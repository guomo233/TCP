# TCP-Stack

Mininet 网络环境下对 TCP 协议栈主要功能的实现，包括：链接建立（三次握手）、链接释放（四次挥手）、可靠传输（超时重传）、流量控制（包含窗口探测）、拥塞控制（快速重传/快速恢复等）

该文档记录一些实现中的细节和坑点

## Socket 端口绑定

- 将 socket hash 到 `listen_table` 只基于 `sport` ；
- `bind_table` 的意义是维护端口占用情况，
服务端因为需要监听端口，需要把 `sport` 同时绑定到 `listen_table`、`bind_table`，
而客户端只将 `sport` 绑定到 `bind_table`（在 connect 时进行）；

## Socket 链接建立（三次握手）

#### 本地IP获取
- 在 main.c 中，利用 Linux 提供的 `getifaddrs` 函数以及 `ifaddrs` 结构体等，
获取本机IP等信息，存放在 `instance->iface_list` 所维护的节点类型为 `iface_info_t` 的链表中，
`iface_list.next` 对应的 `iface_info_t` 就是所需要的 IP；
- 在 `tcp_apps.c` 中调用 `tcp_sock_bind` 时没有将本地IP设置好，而是用 `0.0.0.0` 临时代替，
所以对客户端而言，在 `tcp_sock_connect` 时需要通过 `instance` 获取IP，
对服务器而言，因为IP是在 `tcp_sock_accept` 中绑定，所以直接拿客户端发过来的IP就好了；

#### 初始化 Socket
- 使用 `tcp_sock.c/alloc_tcp_sock` 初始化 socket 时，
利用 `tcp.c` 中的 `tcp_new_iss` 随机生成初始 seq 并保存在 `tsk->iss` 中，
同时让 `snd_nxt` 等于 `tsk->iss`

#### 客户端
- 对于客户端，因为没有指定端口，所以在 `tcp_sock_connect` 绑定时，
用 `tcp_get_port` 获取一个空闲端口；
- 对于客户端，因为在 `main.c` 中将输入的端口都用 `htons` 转化了，
所以在 `tcp_sock_connect` 中也需要再将接收到的端口 `ntohs` 将其转化回来，
对于服务端同理，在 `tcp_sock_bind` 中用 `ntohs` 将其转回来（这样做感觉没有意义，
因为两次转化之间都是在本机做的，没牵扯数据传输）；
- 客户端进入 SYN_SENT 就可以将 socket 放到 `established_table`（即 connect 时既要将 socket 放入 bind_table，
- 又要放入 established_table），
且要在发送 SYN 前将 socket 放入 `established_table`，
因为对客户端而言 socket 没有在 `listen_table` 中，
要想接受数据只能处于 `established_table` 中，
所以为了保证收到 ACK 时 socket 一定在 `established_table` 中需要这么做

#### 服务端
- 当服务端接收到 SYN 后就创建一个 child socket 并将其放入 `listen_queue`，
第二次握手是通过 child socket 来进行的。作用是可以同时维持多个半连接，
一旦其中一个半连接收到 ACK 后就将其放入 `accept_queue` 等待阻塞在 `wait_accpet` 条件上的线程唤醒后从中取，
最多可以往 `accept_queue` 中放 `backlog` 个 `child socket`，但因为正式连接会将其从中取出，
所以不代表该服务器端口只维护这么多用户的连接（可以同时维护很多），
但前提是需要一个线程不断轮询 `tcp_sock_accept` 对每个返回的 child socket 单独创建一个线程来处理，
该项目只是取了一个；
- `tcp_sock.c/tcp_sock_accept_enqueue` 中使用 `!list_empty` 是判断是判断该 child socket 是不是在 `listen_queue` 中，
如果在就将其从 `listen_queue` 中取出；
- 因为在 `tcp_sock_accept_enqueue` 时没有判断是否满，
所以在调用 `tcp_sock_accept_enqueue(csk)` 时应该用 `tcp_sock_accept_queue_full(csk->parent)` 判断是不是已经满了；
- 服务端进入 SYN_RECV 时就可以将 socket 放到 `established_table`；

## Socket 链接释放/断开（四次挥手）

#### 主动释放
- `tcp_sock.c/tcp_sock_close` 在 ESTABLISHED 时，
进入 FIN_WAIT_1 应该发送 FIN 而非 FIN|ACK；
- `tcp_in.c` 中 `tcp_process` 在 TCP_WAIT_2 时，
进入 TCP_TIME_WAIT 应该处理 FIN 而非 FIN|ACK；

#### 被动释放
- `tcp_in.c/tcp_process` 在 ESTABLISHED 收到 FIN 向 CLOSE_WAIT 转移后不应该直接继续转移到 LAST_ACK，
因为可能有未发送完的数据，并且要在转化为 CLOSE_WAIT 时需要将等待在 `wait_recv` 条件的阻塞给唤醒，
让其通过几次循环继续将缓冲区的内容读完，否则会导致接受的文件不完全；
- `tcp_in.c/tcp_process` 在 TCP_TIME_WAIT 时应该像 TCP_FIN_WAIT_2 一样继续处理可能到来的 FIN；

#### 强行释放（RST）
- 任何状态下如果接到 RST，就立马进入 CLOSED 状态，
并将其从 `listen_table` 和 `established_table` 中删除；
- 如果 CLOSED 状态下接受任何数据包都返回 RST，让对方关闭连接；
- 因为 socket 是在 `established_table` 或 `listen_table` 中找的，
如果接收方端口未开放则应该得到 NULL，此时应该发送 RST；
- `tcp_in.c/tcp_process` 接到 RST 时应该要判断当前是客户端还是服务端，
如果是客户端应该要 `tcp_bind_unhash`，
因为 RST 不论是发送方还是接收方都可能会收到；

#### 清理/回收
- 只有在完全进入 CLOSED 才将 socket 从 `established_table`、`listen_table` 中移出，
其中如果是接收方（`tsk->parent` 来判断），无需从 `bind_table` 中移出，
因为连接的是 child socket，而其不在 `bind_table` 中，
如果是发送方，则要从 `bind_table` 中移出。
并且双方都要将 socket 内存回收（通过 `tcp_bind_unhash`、`tcp_unhash` 自动进行）；
- 在定时器中变为 CLOSED 时不仅要从 `bind_table` 移出，
也要从 `established_table` 移出
- `tcp_hash` 时会根据状态放到对应的 table，
而 `tcp_unhash` 之所以不需要根据指定从哪个 table 中 unhash 是因为接收方 parent socket 是放在 listen_table，
而 child socket 是放在 established_table，
所以不存在真正意义上的任意 socket 同时在 listen_table、established_table 中；

## 数据收发


