/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Implementation of the Transmission Control Protocol(TCP).
 *
 * Authors:	Ross Biro
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Mark Evans, <evansmp@uhura.aston.ac.uk>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Florian La Roche, <flla@stud.uni-sb.de>
 *		Charles Hedrick, <hedrick@klinzhai.rutgers.edu>
 *		Linus Torvalds, <torvalds@cs.helsinki.fi>
 *		Alan Cox, <gw4pts@gw4pts.ampr.org>
 *		Matthew Dillon, <dillon@apollo.west.oic.com>
 *		Arnt Gulbrandsen, <agulbra@nvg.unit.no>
 *		Jorge Cwik, <jorge@laser.satlink.net>
 */

#include <linux/module.h>
#include <linux/gfp.h>
#include <net/tcp.h>

/**
 *  tcp_write_err() - close socket and save error info
 *  @sk:  The socket the error has appeared on.
 *
 *  Returns: Nothing (void)
 */

static void tcp_write_err(struct sock *sk)
{
	sk->sk_err = sk->sk_err_soft ? : ETIMEDOUT;
	sk->sk_error_report(sk);

	tcp_write_queue_purge(sk);
	tcp_done(sk);
	__NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPABORTONTIMEOUT);
}

/**
 *  tcp_out_of_resources() - Close socket if out of resources
 *  @sk:        pointer to current socket
 *  @do_reset:  send a last packet with reset flag
 *
 *  Do not allow orphaned sockets to eat all our resources.
 *  This is direct violation of TCP specs, but it is required
 *  to prevent DoS attacks. It is called when a retransmission timeout
 *  or zero probe timeout occurs on orphaned socket.
 *
 *  Also close if our net namespace is exiting; in that case there is no
 *  hope of ever communicating again since all netns interfaces are already
 *  down (or about to be down), and we need to release our dst references,
 *  which have been moved to the netns loopback interface, so the namespace
 *  can finish exiting.  This condition is only possible if we are a kernel
 *  socket, as those do not hold references to the namespace.
 *
 *  Criteria is still not confirmed experimentally and may change.
 *  We kill the socket, if:
 *  1. If number of orphaned sockets exceeds an administratively configured
 *     limit.
 *  2. If we have strong memory pressure.
 *  3. If our net namespace is exiting.
 */
static int tcp_out_of_resources(struct sock *sk, bool do_reset)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int shift = 0;

	/* If peer does not open window for long time, or did not transmit
	 * anything for long time, penalize it. */
	if ((s32)(tcp_jiffies32 - tp->lsndtime) > 2*TCP_RTO_MAX || !do_reset)
		shift++;

	/* If some dubious ICMP arrived, penalize even more. */
	if (sk->sk_err_soft)
		shift++;

	if (tcp_check_oom(sk, shift)) {
		/* Catch exceptional cases, when connection requires reset.
		 *      1. Last segment was sent recently. */
		if ((s32)(tcp_jiffies32 - tp->lsndtime) <= TCP_TIMEWAIT_LEN ||
		    /*  2. Window is closed. */
		    (!tp->snd_wnd && !tp->packets_out))
			do_reset = true;
		if (do_reset)
			tcp_send_active_reset(sk, GFP_ATOMIC);
		tcp_done(sk);
		__NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPABORTONMEMORY);
		return 1;
	}

	if (!check_net(sock_net(sk))) {
		/* Not possible to send reset; just close */
		tcp_done(sk);
		return 1;
	}

	return 0;
}

/**
 *  tcp_orphan_retries() - Returns maximal number of retries on an orphaned socket
 *  @sk:    Pointer to the current socket.
 *  @alive: bool, socket alive state
 */
static int tcp_orphan_retries(struct sock *sk, bool alive)
{
	int retries = sock_net(sk)->ipv4.sysctl_tcp_orphan_retries; /* May be zero. */

	/* We know from an ICMP that something is wrong. */
	if (sk->sk_err_soft && !alive)
		retries = 0;

	/* However, if socket sent something recently, select some safe
	 * number of retries. 8 corresponds to >100 seconds with minimal
	 * RTO of 200msec. */
	if (retries == 0 && alive)
		retries = 8;
	return retries;
}

static void tcp_mtu_probing(struct inet_connection_sock *icsk, struct sock *sk)
{
	const struct net *net = sock_net(sk);
	int mss;

	/* Black hole detection */
	if (!net->ipv4.sysctl_tcp_mtu_probing)
		return;

	if (!icsk->icsk_mtup.enabled) {
		icsk->icsk_mtup.enabled = 1;
		icsk->icsk_mtup.probe_timestamp = tcp_jiffies32;
	} else {
		mss = tcp_mtu_to_mss(sk, icsk->icsk_mtup.search_low) >> 1;
		mss = min(net->ipv4.sysctl_tcp_base_mss, mss);
		mss = max(mss, 68 - tcp_sk(sk)->tcp_header_len);
		icsk->icsk_mtup.search_low = tcp_mss_to_mtu(sk, mss);
	}
	tcp_sync_mss(sk, icsk->icsk_pmtu_cookie);
}


/**
 *  retransmits_timed_out() - returns true if this connection has timed out
 *  @sk:       The current socket
 *  @boundary: max number of retransmissions
 *  @timeout:  A custom timeout value.
 *             If set to 0 the default timeout is calculated and used.
 *             Using TCP_RTO_MIN and the number of unsuccessful retransmits.
 *
 * The default "timeout" value this function can calculate and use
 * is equivalent to the timeout of a TCP Connection
 * after "boundary" unsuccessful, exponentially backed-off
 * retransmissions with an initial RTO of TCP_RTO_MIN.
 */
static bool retransmits_timed_out(struct sock *sk,
				  unsigned int boundary,
				  unsigned int timeout)
{
	const unsigned int rto_base = TCP_RTO_MIN;
	unsigned int linear_backoff_thresh, start_ts;

	if (!inet_csk(sk)->icsk_retransmits)
		return false;

	start_ts = tcp_sk(sk)->retrans_stamp;
	if (unlikely(!start_ts)) {
		struct sk_buff *head = tcp_rtx_queue_head(sk);

		if (!head)
			return false;
		start_ts = tcp_skb_timestamp(head);
	}

	if (likely(timeout == 0)) {
		linear_backoff_thresh = ilog2(TCP_RTO_MAX/rto_base);

		if (boundary <= linear_backoff_thresh)
			timeout = ((2 << boundary) - 1) * rto_base;
		else
			timeout = ((2 << linear_backoff_thresh) - 1) * rto_base +
				(boundary - linear_backoff_thresh) * TCP_RTO_MAX;
	}
	return (tcp_time_stamp(tcp_sk(sk)) - start_ts) >= jiffies_to_msecs(timeout);
}

/* A write timeout has occurred. Process the after effects. */
/*
 * 当发生重传之后,需要检测当前的资源使用
 * 情况.如果重传达到上限则需要立即关闭套接字
 */
static int tcp_write_timeout(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct net *net = sock_net(sk);
	bool expired, do_reset;
	int retry_until;

	/*
	 * 在建立连接阶段超时,则需要检测使用的
	 * 路由缓存项,并获取重试次数的最大值.
	 */
	if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV)) {
		if (icsk->icsk_retransmits) {
			dst_negative_advice(sk);
		} else if (!tp->syn_data && !tp->syn_fastopen) {
			sk_rethink_txhash(sk);
		}
		retry_until = icsk->icsk_syn_retries ? : net->ipv4.sysctl_tcp_syn_retries;
		expired = icsk->icsk_retransmits >= retry_until;
	} else {
		/*
		 * 当重传次数达到sysctl_tcp_retries1时,则需要进行
		 * 黑洞检测.完成黑洞检测后还需检测使用的
		 * 路由缓存项.
		 * 系统启用路径MTU发现时,如果路径MTU发现的
		 * 控制数据块中的开关没有开启,则将其开启,
		 * 并根据PMTU同步MSS.否则,将当前路径MTU发现
		 * 区间左端点的一半作为新区间的左端点重新
		 * 设定路径MTU发现区间,并根据路径MTU同步MSS.
		 */
		if (retransmits_timed_out(sk, net->ipv4.sysctl_tcp_retries1, 0)) {
			/* Black hole detection */
			tcp_mtu_probing(icsk, sk);

			dst_negative_advice(sk);
		} else {
			sk_rethink_txhash(sk);
		}

		/*
		 * 如果当前套接字连接已断开并即将关闭,则
		 * 需要对当前使用的资源进行检测.当前的孤
		 * 儿套接字数量达到sysctl_tcp_max_orphans或者当前
		 * 已使用内存达到硬性限制时,需要即刻关闭
		 * 该套接字,这虽然不符合TCP的规范,但为了防
		 * 止DoS攻击必须这么处理.
		 */
		retry_until = net->ipv4.sysctl_tcp_retries2;
		if (sock_flag(sk, SOCK_DEAD)) {
			const bool alive = icsk->icsk_rto < TCP_RTO_MAX;

			retry_until = tcp_orphan_retries(sk, alive);
			do_reset = alive ||
				!retransmits_timed_out(sk, retry_until, 0);

			if (tcp_out_of_resources(sk, do_reset))
				return 1;
		}
		expired = retransmits_timed_out(sk, retry_until,
						icsk->icsk_user_timeout);
	}
	tcp_fastopen_active_detect_blackhole(sk, expired);

	if (BPF_SOCK_OPS_TEST_FLAG(tp, BPF_SOCK_OPS_RTO_CB_FLAG))
		tcp_call_bpf_3arg(sk, BPF_SOCK_OPS_RTO_CB,
				  icsk->icsk_retransmits,
				  icsk->icsk_rto, (int)expired);

	/*
	 * 当重传次数达到建立连接重传上限、超时
	 * 重传上限或确认连接异常期间重试上限这
	 * 三种上限之一时，都必须关闭套接字，并
	 * 且需要报告相应的错误。
	 */
	if (expired) {
		/* Has it gone just too far? */
		tcp_write_err(sk);
		return 1;
	}

	return 0;
}

/* Called with BH disabled */
void tcp_delack_timer_handler(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);

	 /*
	 * 回收缓存
	 */
	sk_mem_reclaim_partial(sk);

	/*
	 * 如果TCP状态为CLOSE，或者没有启动延时发送
	 * ACK定时器，则无需作进一步处理。
	 */
	if (((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN)) ||
	    !(icsk->icsk_ack.pending & ICSK_ACK_TIMER))
		goto out;

	 /*
	 * 如果超时时间还未到，则重新复位定时器，
	 * 然后退出。
	 */
	if (time_after(icsk->icsk_ack.timeout, jiffies)) {
		sk_reset_timer(sk, &icsk->icsk_delack_timer, icsk->icsk_ack.timeout);
		goto out;
	}
	/*
	 * 检测完成后，正式进入延迟确认处理之前，
	 * 需去掉ICSK_ACK_TIMER标志。
	 */
	icsk->icsk_ack.pending &= ~ICSK_ACK_TIMER;

	/*
	 * 如果此时有ACK需要发送，则调用tcp_send_ack()构造
	 * 并发送ACK段，在发送ACK段之前需先离开pingpong
	 * 模式，并重新设定延时确认的估算值。
	 */
	if (inet_csk_ack_scheduled(sk)) {
		if (!icsk->icsk_ack.pingpong) {
			/* Delayed ACK missed: inflate ATO. */
			icsk->icsk_ack.ato = min(icsk->icsk_ack.ato << 1, icsk->icsk_rto);
		} else {
			/* Delayed ACK missed: leave pingpong mode and
			 * deflate ATO.
			 */
			icsk->icsk_ack.pingpong = 0;
			icsk->icsk_ack.ato      = TCP_ATO_MIN;
		}
		tcp_mstamp_refresh(tcp_sk(sk));
		tcp_send_ack(sk);
		__NET_INC_STATS(sock_net(sk), LINUX_MIB_DELAYEDACKS);
	}

out:
	if (tcp_under_memory_pressure(sk))
		sk_mem_reclaim(sk);
}


/**
 *  tcp_delack_timer() - The TCP delayed ACK timeout handler
 *  @data:  Pointer to the current socket. (gets casted to struct sock *)
 *
 *  This function gets (indirectly) called when the kernel timer for a TCP packet
 *  of this socket expires. Calls tcp_delack_timer_handler() to do the actual work.
 *
 *  Returns: Nothing (void)
 */
/*
 * 延时ACK"定时器在TCP收到必须被确认但无需马上发出
 * 确认的段时设定，TCP在200ms后发送确认响应。如果在
 * 这200ms内，有数据要在该连接上发送，延时ACK响应就
 * 可随数据一起发送回对端，称为捎带确认。
 */
static void tcp_delack_timer(struct timer_list *t)
{
	struct inet_connection_sock *icsk =
			from_timer(icsk, t, icsk_delack_timer);
	struct sock *sk = &icsk->icsk_inet.sk;

	bh_lock_sock(sk);
	/*
	 * 如果传输控制块已被用户进程锁定，
	 * 则此时不能作处理，只是重新设置
	 * 定时器超时时间，同时设置blocked
	 * 标记。
	 */
	if (!sock_owned_by_user(sk)) {
		tcp_delack_timer_handler(sk);
	} else {
		icsk->icsk_ack.blocked = 1;
		__NET_INC_STATS(sock_net(sk), LINUX_MIB_DELAYEDACKLOCKED);
		/* deleguate our work to tcp_release_cb() */
		if (!test_and_set_bit(TCP_DELACK_TIMER_DEFERRED, &sk->sk_tsq_flags))
			sock_hold(sk);
	}
	bh_unlock_sock(sk);
	sock_put(sk);
}

/*
 * "持续"定时器在对端通告接收窗口为0，阻止TCP继续发送
 * 数据时设定。由于连接对端发送的窗口通告不可靠(只有
 * 数据才会确认，ACK不会确认)，允许TCP继续发送数据的后
 * 续窗口更新有可能丢失，因此，如果TCP有数据发送，而
 * 对端通告接收窗口为0，则持续定时器启动，超时后向
 * 对端发送1字节的数据，以判断对端接收窗口是否已打开。
 * 与重传定时器类似，持续定时器的超时值也是动态计算的，
 * 取决于连接的往返时间，在5~60s之间取值。
 * tcp_probe_timer()为持续定时器超时的处理函数。探测定时器就是当接收到对端的window为0的时候，需要探测对端窗口是否变大，
 */ 
//真正的probe报文发送在tcp_send_probe0中的tcp_write_wakeup  
//tcp_write_timer包括数据报重传tcp_retransmit_timer和窗口探测定时器tcp_probe_timer
static void tcp_probe_timer(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct sk_buff *skb = tcp_send_head(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	int max_probes;
	u32 start_ts;

	if (tp->packets_out || !skb) {
		icsk->icsk_probes_out = 0;
		return;
	}

	/* RFC 1122 4.2.2.17 requires the sender to stay open indefinitely as
	 * long as the receiver continues to respond probes. We support this by
	 * default and reset icsk_probes_out with incoming ACKs. But if the
	 * socket is orphaned or the user specifies TCP_USER_TIMEOUT, we
	 * kill the socket when the retry count and the time exceeds the
	 * corresponding system limit. We also implement similar policy when
	 * we use RTO to probe window in tcp_retransmit_timer().
	 */
	start_ts = tcp_skb_timestamp(skb);
	if (!start_ts)
		skb->skb_mstamp = tp->tcp_mstamp;
	else if (icsk->icsk_user_timeout &&
		 (s32)(tcp_time_stamp(tp) - start_ts) >
		 jiffies_to_msecs(icsk->icsk_user_timeout))
		goto abort;

	max_probes = sock_net(sk)->ipv4.sysctl_tcp_retries2;
	/*
	 * 处理连接已断开，套接字即将关闭的情况
	 */
	if (sock_flag(sk, SOCK_DEAD)) {
		 /*
		 * TCP协议规定RTT的最大值为120s(TCP_RTO_MAX)，因此
		 * 可以通过将指数退避算法得出的超时时间与
		 * RTT最大值相比，来判断是否需要给对方发送
		 * RST。
		 */
		const bool alive = inet_csk_rto_backoff(icsk, TCP_RTO_MAX) < TCP_RTO_MAX;

		 /*
		 * 如果连接已断开，套接字即将关闭，则获取在
		 * 关闭本端TCP连接前重试次数的上限。
		 */
		max_probes = tcp_orphan_retries(sk, alive);
		if (!alive && icsk->icsk_backoff >= max_probes)
			goto abort;
		 /*
		 * 释放资源，如果该套接字在释放过程中被关闭，
		 * 就无需再发送持续探测段了。
		 */
		if (tcp_out_of_resources(sk, true))
			return;
	}

	if (icsk->icsk_probes_out > max_probes) {
abort:		tcp_write_err(sk);
	} else {
		/* Only send another probe if we didn't close things up. */
		/*
		 * 否则，再一次发送持续定时器。
		 */
		tcp_send_probe0(sk);
	}
}

/*
 *	Timer for Fast Open socket to retransmit SYNACK. Note that the
 *	sk here is the child socket, not the parent (listener) socket.
 */
static void tcp_fastopen_synack_timer(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	int max_retries = icsk->icsk_syn_retries ? :
	    sock_net(sk)->ipv4.sysctl_tcp_synack_retries + 1; /* add one more retry for fastopen */
	struct request_sock *req;

	req = tcp_sk(sk)->fastopen_rsk;
	req->rsk_ops->syn_ack_timeout(req);

	if (req->num_timeout >= max_retries) {
		tcp_write_err(sk);
		return;
	}
	/* XXX (TFO) - Unlike regular SYN-ACK retransmit, we ignore error
	 * returned from rtx_syn_ack() to make it more persistent like
	 * regular retransmit because if the child socket has been accepted
	 * it's not good to give up too easily.
	 */
	inet_rtx_syn_ack(sk, req);
	req->num_timeout++;
	icsk->icsk_retransmits++;
	inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
			  TCP_TIMEOUT_INIT << req->num_timeout, TCP_RTO_MAX);
}


/**
 *  tcp_retransmit_timer() - The TCP retransmit timeout handler
 *  @sk:  Pointer to the current socket.
 *
 *  This function gets called when the kernel timer for a TCP packet
 *  of this socket expires.
 *
 *  It handles retransmission, timer adjustment and other necesarry measures.
 *
 *  Returns: Nothing (void)
 */
//tcp_write_timer包括数据报重传tcp_retransmit_timer和窗口探测定时器tcp_probe_timer
//见tcp_event_new_data_sent，prior_packets为0时才会重启定时器,而prior_packets则是发送未确认的段的个数,也就是说如果发送了很多段,如果前面的段没有确认,那么后面发送的时候不会重启这个定时器.
//tcp_rearm_rto ///为0说明所有的传输的段都已经acked。此时remove定时器。否则重启定时器。  
void tcp_retransmit_timer(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct net *net = sock_net(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);

	if (tp->fastopen_rsk) {
		WARN_ON_ONCE(sk->sk_state != TCP_SYN_RECV &&
			     sk->sk_state != TCP_FIN_WAIT1);
		tcp_fastopen_synack_timer(sk);
		/* Before we receive ACK to our SYN-ACK don't retransmit
		 * anything else (e.g., data or FIN segments).
		 */
		return;
	}
	/*
	 * 如果此时从发送队列输出的段都已
	 * 得到了确认,则无需重传处理.
	 */
	if (!tp->packets_out)
		goto out;

	WARN_ON(tcp_rtx_queue_empty(sk));

	tp->tlp_high_seq = 0;

	/*
	 * 在重传过程中，如果超时重传超时上限TCP_RTO_MAX(120s)还没有接收
	 * 到对方的确认，则认为有错误发生，调用tcp_write_err()报告错误并
	 * 关闭套接字，然后返回；否则TCP进入拥塞控制的LOSS状态，并重新
	 * 传送重传队列中的第一个段。
	 */
	if (!tp->snd_wnd && !sock_flag(sk, SOCK_DEAD) &&
	    !((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))) {
		/* Receiver dastardly shrinks window. Our retransmits
		 * become zero probes, but we should not timeout this
		 * connection. If the socket is an orphan, time it out,
		 * we cannot allow such beasts to hang infinitely.
		 */
		struct inet_sock *inet = inet_sk(sk);
		if (sk->sk_family == AF_INET) {
			net_dbg_ratelimited("Peer %pI4:%u/%u unexpectedly shrunk window %u:%u (repaired)\n",
					    &inet->inet_daddr,
					    ntohs(inet->inet_dport),
					    inet->inet_num,
					    tp->snd_una, tp->snd_nxt);
		}
#if IS_ENABLED(CONFIG_IPV6)
		else if (sk->sk_family == AF_INET6) {
			net_dbg_ratelimited("Peer %pI6:%u/%u unexpectedly shrunk window %u:%u (repaired)\n",
					    &sk->sk_v6_daddr,
					    ntohs(inet->inet_dport),
					    inet->inet_num,
					    tp->snd_una, tp->snd_nxt);
		}
#endif
		 /*
		 * 在重传过程中，如果超时重传超时上限TCP_RTO_MAX(120s)还没有接收
		 * 到对方的确认，则认为有错误发生，调用tcp_write_err()报告错误并
		 * 关闭套接字，然后返回；否则TCP进入拥塞控制的LOSS状态，并重新
		 * 传送重传队列中的第一个段。
		 */
		if (tcp_jiffies32 - tp->rcv_tstamp > TCP_RTO_MAX) {
			tcp_write_err(sk);
			goto out;
		}
		tcp_enter_loss(sk);
		tcp_retransmit_skb(sk, tcp_rtx_queue_head(sk), 1);
		/*
		 * 由于发生了重传，传输控制块中的路由缓存项需更新，
		 * 因此将其清除，最后跳转到out_reset_timer标签处处理。
		 */
		__sk_dst_reset(sk);
		goto out_reset_timer;
	}

	//走到下面说明是处于连接建立阶段或者对方的滑动窗口为0了


    /*
	 * 当发生重传之后,需要检测当前的资源使用
	 * 情况和重传的次数.如果重传次数达到上限,
	 * 则需要报告错误并强行关闭套接字.如果只
	 * 是使用的资源达到使用的上限,则不进行此
	 * 次重传.
	 */
	if (tcp_write_timeout(sk))
		goto out;

	/*
	 * 如果重传次数为0,说明刚进入重传阶段,则
	 * 根据不同的拥塞状态进行相关的数据统计.   第一次重传可能是对方滑动窗口满，需要进行拥塞控制
	 */
	if (icsk->icsk_retransmits == 0) {
		int mib_idx;

		if (icsk->icsk_ca_state == TCP_CA_Recovery) {
			if (tcp_is_sack(tp))
				mib_idx = LINUX_MIB_TCPSACKRECOVERYFAIL;
			else
				mib_idx = LINUX_MIB_TCPRENORECOVERYFAIL;
		} else if (icsk->icsk_ca_state == TCP_CA_Loss) {
			mib_idx = LINUX_MIB_TCPLOSSFAILURES;
		} else if ((icsk->icsk_ca_state == TCP_CA_Disorder) ||
			   tp->sacked_out) {
			if (tcp_is_sack(tp))
				mib_idx = LINUX_MIB_TCPSACKFAILURES;
			else
				mib_idx = LINUX_MIB_TCPRENOFAILURES;
		} else {
			mib_idx = LINUX_MIB_TCPTIMEOUTS;
		}
		__NET_INC_STATS(sock_net(sk), mib_idx);
	}

	// 调用tcp_enter_loss()进入常规的RTO慢启动重传恢复阶段
	tcp_enter_loss(sk);

	 /*
	 * 如果发送重传队列上的第一个SKB失败,则复位
	 * 重传定时器,等待下次重传.
	 */
	if (tcp_retransmit_skb(sk, tcp_rtx_queue_head(sk), 1) > 0) {
		/* Retransmission failed because of local congestion,
		 * do not backoff.
		 */
		if (!icsk->icsk_retransmits)
			icsk->icsk_retransmits = 1;
		inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
					  min(icsk->icsk_rto, TCP_RESOURCE_PROBE_INTERVAL),
					  TCP_RTO_MAX);
		goto out;
	}

	/* Increase the timeout each time we retransmit.  Note that
	 * we do not increase the rtt estimate.  rto is initialized
	 * from rtt, but increases here.  Jacobson (SIGCOMM 88) suggests
	 * that doubling rto each time is the least we can get away with.
	 * In KA9Q, Karn uses this for the first few times, and then
	 * goes to quadratic.  netBSD doubles, but only goes up to *64,
	 * and clamps at 1 to 64 sec afterwards.  Note that 120 sec is
	 * defined in the protocol as the maximum possible RTT.  I guess
	 * we'll have to use something other than TCP to talk to the
	 * University of Mars.
	 *
	 * PAWS allows us longer timeouts and large windows, so once
	 * implemented ftp to mars will work nicely. We will have to fix
	 * the 120 second clamps though!
	 */
	/*
	 * 发送成功后,递增指数退避算法指数icsk_backoff
	 * 和累计重传次数icsk_retransmits.
	 */
	icsk->icsk_backoff++;
	icsk->icsk_retransmits++;

out_reset_timer:
	/* If stream is thin, use linear timeouts. Since 'icsk_backoff' is
	 * used to reset timer, set to 0. Recalculate 'icsk_rto' as this
	 * might be increased if the stream oscillates between thin and thick,
	 * thus the old value might already be too high compared to the value
	 * set by 'tcp_set_rto' in tcp_input.c which resets the rto without
	 * backoff. Limit to TCP_THIN_LINEAR_RETRIES before initiating
	 * exponential backoff behaviour to avoid continue hammering
	 * linear-timeout retransmissions into a black hole
	 */
	if (sk->sk_state == TCP_ESTABLISHED &&
	    (tp->thin_lto || net->ipv4.sysctl_tcp_thin_linear_timeouts) &&
	    tcp_stream_is_thin(tp) &&
	    icsk->icsk_retransmits <= TCP_THIN_LINEAR_RETRIES) {
		icsk->icsk_backoff = 0;
		icsk->icsk_rto = min(__tcp_set_rto(tp), TCP_RTO_MAX);
	} else {
		/* Use normal (exponential) backoff */
		icsk->icsk_rto = min(icsk->icsk_rto << 1, TCP_RTO_MAX);
	}
	/*
	 * 完成重传后,需要设重传超时时间,然后复位重传
	 * 定时器,等待下次重传.
	 */
	inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS, icsk->icsk_rto, TCP_RTO_MAX);
	if (retransmits_timed_out(sk, net->ipv4.sysctl_tcp_retries1 + 1, 0))
		__sk_dst_reset(sk);

out:;
}

/* Called with bottom-half processing disabled.
   Called by tcp_write_timer() */
void tcp_write_timer_handler(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	int event;

	if (((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN)) ||
	    !icsk->icsk_pending)
		goto out;

	if (time_after(icsk->icsk_timeout, jiffies)) {
		sk_reset_timer(sk, &icsk->icsk_retransmit_timer, icsk->icsk_timeout);
		goto out;
	}

	tcp_mstamp_refresh(tcp_sk(sk));
	event = icsk->icsk_pending;

	switch (event) {
	case ICSK_TIME_REO_TIMEOUT:
		tcp_rack_reo_timeout(sk);
		break;
	case ICSK_TIME_LOSS_PROBE:
		tcp_send_loss_probe(sk);
		break;
	case ICSK_TIME_RETRANS:
		icsk->icsk_pending = 0;
		tcp_retransmit_timer(sk);
		break;
	case ICSK_TIME_PROBE0:
		icsk->icsk_pending = 0;
		tcp_probe_timer(sk);
		break;
	}

out:
	sk_mem_reclaim(sk);
}

 /*
 * 重传定时器在TCP发送数据时设定，如果定时器
 * 已超时而对端确认还未到达，则TCP将重传数据。
 * 重传定时器的超时时间值是动态计算的，取决于
 * TCP为该连接测量的往返时间以及该段已被重传
 * 的次数。
 */ 
//tcp_write_timer包括数据报重传tcp_retransmit_timer和窗口探测定时器tcp_probe_timer
static void tcp_write_timer(struct timer_list *t)
{
	struct inet_connection_sock *icsk =
			from_timer(icsk, t, icsk_retransmit_timer);
	struct sock *sk = &icsk->icsk_inet.sk;

	bh_lock_sock(sk);
	if (!sock_owned_by_user(sk)) {
		tcp_write_timer_handler(sk);
	} else {
		/* delegate our work to tcp_release_cb() */
		if (!test_and_set_bit(TCP_WRITE_TIMER_DEFERRED, &sk->sk_tsq_flags))
			sock_hold(sk);
	}
	bh_unlock_sock(sk);
	sock_put(sk);
}

void tcp_syn_ack_timeout(const struct request_sock *req)
{
	struct net *net = read_pnet(&inet_rsk(req)->ireq_net);

	__NET_INC_STATS(net, LINUX_MIB_TCPTIMEOUTS);
}
EXPORT_SYMBOL(tcp_syn_ack_timeout);

void tcp_set_keepalive(struct sock *sk, int val)
{
	if ((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN))
		return;

	if (val && !sock_flag(sk, SOCK_KEEPOPEN))
		inet_csk_reset_keepalive_timer(sk, keepalive_time_when(tcp_sk(sk)));
	else if (!val)
		inet_csk_delete_keepalive_timer(sk);
}
EXPORT_SYMBOL_GPL(tcp_set_keepalive);


static void tcp_keepalive_timer (struct timer_list *t)
{
	struct sock *sk = from_timer(sk, t, sk_timer);
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	u32 elapsed;

	/* Only process if socket is not in use. */
	bh_lock_sock(sk);
	if (sock_owned_by_user(sk)) {
		/* Try again later. */
		inet_csk_reset_keepalive_timer (sk, HZ/20);
		goto out;
	}

	if (sk->sk_state == TCP_LISTEN) {
		pr_err("Hmm... keepalive on a LISTEN ???\n");
		goto out;
	}

	tcp_mstamp_refresh(tp);
	if (sk->sk_state == TCP_FIN_WAIT2 && sock_flag(sk, SOCK_DEAD)) {
		if (tp->linger2 >= 0) {
			const int tmo = tcp_fin_time(sk) - TCP_TIMEWAIT_LEN;

			if (tmo > 0) {
				tcp_time_wait(sk, TCP_FIN_WAIT2, tmo);
				goto out;
			}
		}
		tcp_send_active_reset(sk, GFP_ATOMIC);
		goto death;
	}

	if (!sock_flag(sk, SOCK_KEEPOPEN) ||
	    ((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_SYN_SENT)))
		goto out;

	elapsed = keepalive_time_when(tp);

	/* It is alive without keepalive 8) */
	if (tp->packets_out || !tcp_write_queue_empty(sk))
		goto resched;

	elapsed = keepalive_time_elapsed(tp);

	if (elapsed >= keepalive_time_when(tp)) {
		/* If the TCP_USER_TIMEOUT option is enabled, use that
		 * to determine when to timeout instead.
		 */
		if ((icsk->icsk_user_timeout != 0 &&
		    elapsed >= icsk->icsk_user_timeout &&
		    icsk->icsk_probes_out > 0) ||
		    (icsk->icsk_user_timeout == 0 &&
		    icsk->icsk_probes_out >= keepalive_probes(tp))) {
			tcp_send_active_reset(sk, GFP_ATOMIC);
			tcp_write_err(sk);
			goto out;
		}
		if (tcp_write_wakeup(sk, LINUX_MIB_TCPKEEPALIVE) <= 0) {
			icsk->icsk_probes_out++;
			elapsed = keepalive_intvl_when(tp);
		} else {
			/* If keepalive was lost due to local congestion,
			 * try harder.
			 */
			elapsed = TCP_RESOURCE_PROBE_INTERVAL;
		}
	} else {
		/* It is tp->rcv_tstamp + keepalive_time_when(tp) */
		elapsed = keepalive_time_when(tp) - elapsed;
	}

	sk_mem_reclaim(sk);

resched:
	inet_csk_reset_keepalive_timer (sk, elapsed);
	goto out;

death:
	tcp_done(sk);

out:
	bh_unlock_sock(sk);
	sock_put(sk);
}

// TCP定时器初始化
void tcp_init_xmit_timers(struct sock *sk)
{
	inet_csk_init_xmit_timers(sk, &tcp_write_timer, &tcp_delack_timer,
				  &tcp_keepalive_timer);
	hrtimer_init(&tcp_sk(sk)->pacing_timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_ABS_PINNED);
	tcp_sk(sk)->pacing_timer.function = tcp_pace_kick;
}
