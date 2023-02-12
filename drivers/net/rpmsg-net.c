/**
 * @file rpmsg_net.c
 *
 * @brief RPMsg Network Driver
 *
 * This Linux kernel module exchanges Ethernet frames over the RPMsg bus, providing a virtual
 * point-to-point network connection between two cores. The virtual network interface is named
 * rpmsgN, where N starts at 0.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/rpmsg.h>
#include <linux/pkt_sched.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/workqueue.h>

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("James Tavares <james.tavares@gmail.com>");
MODULE_DESCRIPTION("point-to-point network interface over RPMsg bus");

// Module parameters
static int debug = 3;
module_param(debug, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(debug, "debug level, with 0 = none, 1 = rx/tx events, 2 = packet payload, 3 = insane");

enum DBG_LEVEL {
    DBG_NONE,
    DBG_RX_TX,
    DBG_PACKET,
    DBG_INSANE,
};

// Defines

// Linux's RPMsg stack doesn't export MAX_RPMSG_BUF_SIZE or sizeof(rpmsg_hdr),
// so we need to hard code it here. Ugh. Keep this in sync with RPMsg implementation!
// NOTE: we have modified the kernel's RPMsg stack to use buffers of 2048 bytes
#define MAX_RPMSG_BUF_SIZE          2048
#define RPMSG_HEADER_SIZE           16
#define MAX_RPMSG_PAYLOAD_SIZE      (MAX_RPMSG_BUF_SIZE-RPMSG_HEADER_SIZE)

// The maximum number of simultaneous rpmsg_net instances this driver supports.
#define RPMSG_NET_MAX_DEVICES       8

// RPMsg MTU is lesser of the max RPMsg payload size, or 1500 (standard ethernet)
#define RPMSG_NET_L2_MTU            (MAX_RPMSG_PAYLOAD_SIZE > 1500 ? 1500 : MAX_RPMSG_PAYLOAD_SIZE)

// Size of the layer 2 header. For now, we just use full ethernet header. TODO: consider optimizing?
// Note: ETH_HLEN does not include room for a VLAN identifier. We do not support VLANs.
#define RPMSG_NET_L2_HEADER_SIZE    ETH_HLEN

// Checksum size
#define RPMSG_NET_L2_CHECKSUM_SIZE  ETH_FCS_LEN

// Pad the layer 2 header so that the IP header starts on a 16 byte boundary
#define RPMSG_NET_SKB_PADDING_SIZE  (16 - (RPMSG_NET_L2_HEADER_SIZE & 0xf))

// Prototypes - RPMsg Driver API
static int rpmsg_probe(struct rpmsg_device *rpdev);
static int rpmsg_callback(struct rpmsg_device *rpdev, void *data, int len, void *priv, u32 src);
static void rpmsg_remove(struct rpmsg_device *rpdev);

// Prototypes - Network Driver API
static void net_setup(struct net_device *ndev);
static int net_open(struct net_device *ndev);
static int net_close(struct net_device *ndev);
static void net_xmit_delayed_work_handler(struct work_struct *work);
static void net_xmit_work_handler(struct work_struct *work);
static int net_xmit(struct sk_buff *skb, struct net_device *ndev);

// Prototypes - Module API
static int __init rpmsg_net_module_init(void);
static void __exit rpmsg_net_module_exit(void);

/** @brief rpmsg_net driver instance state */
struct rpmsg_net_priv {
    /**
     * Driver index number. Index into rpmsg_net_device[] array, and basis for `rpmsgN` interface
     * name.
     */
    int idx;

    /** Pointer to the RPMsg stack device handle */
    struct rpmsg_device *rpdev;

    /** Pointer to the network device stack device handle */
    struct net_device *ndev;

    /**
     * A work queue to process net device transmit events (Net->RPMsg) packets in a process context,
     * rather than the ISR context provided by the net xmit callback.
     */
    struct work_struct immediate;

    /**
     * A work queue to provide for net device transmit retries in the event that the RPMsg ring
     * buffer is full at the time we attempt to transmit.
     */
    struct delayed_work delayed;

    /** The current skb that is being transmitted from net device to RPMsg */
    struct sk_buff *skb;

    /** A boolean indicating that we are retrying a failed skb transmission */
    bool is_delayed;

    /** A lock for the net device transmitter. Used to avoid race condition on shutdown */
    spinlock_t shutdown_lock;

    /** A flag indicating whether the interace is shutdown, or not */
    bool is_shutdown;
};

/**
 * rpmsg_net driver instance state pointers, by device index
 */
static struct rpmsg_net_priv *rpmsg_net_device[RPMSG_NET_MAX_DEVICES];


//////////////////////
// RPMsg Driver API //
//////////////////////


/** Registration table for RPMsg back-end */
static struct rpmsg_device_id rpmsg_net_id_table[] = {
    { .name = "rpmsg-net" },
    { }
};

MODULE_DEVICE_TABLE(rpmsg, rpmsg_net_id_table);

/** RPMsg operations object */
static struct rpmsg_driver rpmsg_net_client = {
    .drv.name   = KBUILD_MODNAME,
    .id_table   = rpmsg_net_id_table,
    .probe      = rpmsg_probe,
    .callback   = rpmsg_callback,
    .remove     = rpmsg_remove,
};

/**
 * @brief Called by the RPMsg stack upon RPMsg device attachment. Creates and registers the
 *        corresponding `rpmsgN` virtual network device.
 *
 * @param rpdev RPMsg device
 *
 * @return 0 on success, -1 on error
 */
static int rpmsg_probe(struct rpmsg_device *rpdev)
{
    char dummy_payload[] = "hello";
    struct net_device *ndev = NULL;
    struct rpmsg_net_priv *priv = NULL;
    char name[IFNAMSIZ];
    int i;
    int err;

    // find an open rpmsg_net_device slot
    for (i = 0; i<RPMSG_NET_MAX_DEVICES; i++) {
        if (rpmsg_net_device[i] == NULL)
            break;
    }

    if (i == RPMSG_NET_MAX_DEVICES) {
        dev_err(&rpdev->dev, "unable to bind channel 0x%x -> 0x%x: reached maximum rpmsg_net device count of %d\n",
                rpdev->src, rpdev->dst, RPMSG_NET_MAX_DEVICES);
        // TODO return real error here
        return -1;
    }

    // allocate the network device
    snprintf(name, sizeof(name), "rpmsg%d", i);

    ndev = alloc_netdev(sizeof(*priv), name, NET_NAME_UNKNOWN, net_setup);
    if (ndev == NULL) {
        dev_err(&rpdev->dev, "unable to bind channel 0x%x -> 0x%x: unable to allocate netdevice\n",
                rpdev->src, rpdev->dst);
        // TODO return real error here
        return -1;
    }

    // setup our private area, and let the network driver know of rpmsg driver
    priv = netdev_priv(ndev);
    memset(priv, 0x00, sizeof(*priv));
    priv->idx = i;
    priv->ndev = ndev;
    priv->rpdev = rpdev;
    INIT_WORK(&priv->immediate, net_xmit_work_handler);
    INIT_DELAYED_WORK(&priv->delayed, net_xmit_delayed_work_handler);
    priv->skb = NULL;
    priv->is_delayed = false;
    spin_lock_init(&priv->shutdown_lock);
    priv->is_shutdown = false;

    // let RPMsg driver know of the network driver
    rpdev->ept->priv = priv;

    // hard code mac addr as 0x02fcfcfcfcfc
    // (a locally-administered address: see http://www.noah.org/wiki/MAC_address)
    // TODO: get rid of mac address; this is a PTP link; maybe set addr_len=0, etc
//     memset(ndev->dev_addr, 0xfc, ETH_ALEN);
//     ndev->dev_addr[0] = 0x02;
        u8 addr[ETH_ALEN];
        addr[0] = 0x00;
	addr[1] = 0x01;
	addr[2] = 0x00;
	addr[3] = 0x01;
	addr[4] = 0X08;
	addr[5] = 0xff;
	eth_hw_addr_set(ndev, addr);


    eth_random_addr(ndev->perm_addr);


    // RPMsg remote side expects a fake first packet to learn the master node's return address.
    // The remote end will discard this packet
//     err = rpmsg_send(rpdev->ept, dummy_payload, strlen(dummy_payload));
//     if (err) {
//         dev_err(&rpdev->dev, "initial rpmsg_send failed: %d", err);
//         free_netdev(ndev);
//         ndev = NULL;
//         return err;
//     }

    // register the net device
    register_netdev(ndev);

    // success
    dev_info(&rpdev->dev, "new channel: 0x%x -> 0x%x bound to interface %s\n",
             rpdev->src, rpdev->dst, ndev->name);

    rpmsg_net_device[i] = priv;

    return 0;
}

/**
 * @brief Called when an Ethernet frame has been received from the remote processor. Injects the
 *        frame into the virtual network interface's input queue.
 *
 * @param rpdev   RPMsg device
 * @param data    RPMsg payload (incoming Ethernet frame)
 * @param len     Length of the payload
 * @param priv_in Private data (pointer to rpmsg_net_priv instance)
 * @param src     Source RPMsg address
 *
 * @return 0 on success, -1 on error
 */
static int rpmsg_callback(struct rpmsg_device *rpdev, void *data, int len, void *priv_in, u32 src)
{
    unsigned int maxLen;
    struct rpmsg_net_priv *priv;
    struct sk_buff *skb;

    (void)src;

    if (debug >= DBG_INSANE) {
        printk(KERN_INFO "rpmsg_callback: in_interrupt:%lu [in_irq:%lu in_softirq:%lu] in_atomic:%u in_task:%u\n",
               in_interrupt(), in_irq(), in_softirq(), in_atomic(), in_task());
    }

    if (debug >= DBG_RX_TX) {
        dev_info(&rpdev->dev, "recv packet on channel 0x%x -> 0x%x, len:%d\n",
                 rpdev->src, rpdev->dst, len);
    }

    if (debug >= DBG_PACKET) {
        // TODO print packet data
    }

    if (priv_in == NULL) {
        dev_err(&rpdev->dev, "ignoring callback on channel 0x%x -> 0x%x with NULL private pointer\n",
                rpdev->src, rpdev->dst);
        return -1;
    }

    priv = (struct rpmsg_net_priv*)priv_in;

    if (priv->ndev == NULL) {
        dev_err(&rpdev->dev, "ignoring callback on channel 0x%x -> 0x%x with no netdev interface\n",
                rpdev->src, rpdev->dst);
        return -1;
    }

    // Sanity check RPMsg payload length vs. maximum-permitted length for our net device
    maxLen = RPMSG_NET_L2_HEADER_SIZE + READ_ONCE(priv->ndev->mtu) + RPMSG_NET_L2_CHECKSUM_SIZE;
    if (len > maxLen) {
        dev_err(&rpdev->dev, "ignoring packet on channel 0x%x -> 0x%x with len %d > %d\n",
                rpdev->src, rpdev->dst, len, maxLen);
        return -1;
    }

    // silenty drop packet if interface is closed
    if (netif_queue_stopped(priv->ndev)) {
        return 0;
    }

    // TODO stats (rx bytes)

    skb = dev_alloc_skb(RPMSG_NET_SKB_PADDING_SIZE + RPMSG_NET_L2_HEADER_SIZE + RPMSG_NET_L2_MTU);
    if (skb == NULL) {
        dev_err(&rpdev->dev, "out of memory, dropping packet");
        // TODO stats (dropped rx packet)
        return -1;
    }

    skb_reserve(skb, RPMSG_NET_SKB_PADDING_SIZE); // Align IP on 16 byte boundary

    skb->dev = priv->ndev;
    memcpy(skb_put(skb, len), data, len);
    skb->protocol = eth_type_trans(skb, priv->ndev);

    // inject incoming RPMsg frame into the network device's input queue (non-interrupt context)
    netif_rx(skb);

    //TODO stats (rx packet)

    dev_info(&rpdev->dev, "rx done\r\n");
    return 0;
}

/**
 * @brief Called when an RPMsg device has been removed. Shuts down the network interface and
 *        cleans up resources.
 *
 * @param rpdev RPMsg device
 */
static void rpmsg_remove(struct rpmsg_device *rpdev)
{
    struct rpmsg_net_priv *priv = (struct rpmsg_net_priv*)rpdev->ept->priv;
    unsigned long flags;

    // prevent any in-flight transmissions from being injected into the work queue
    spin_lock_irqsave(&priv->shutdown_lock, flags);
    priv->is_shutdown = true;
    netif_stop_queue(priv->ndev);
    spin_unlock_irqrestore(&priv->shutdown_lock, flags);

    // invariant: since is_shutdown=true and we are on other side of critical section, all work has
    //            already been scheduled, or will not be scheduled (frame dropped).

    // cancel all outstanding scheduled work and/or wait for existing work to finish
    cancel_delayed_work_sync(&priv->delayed);
    cancel_work_sync(&priv->immediate);

    // invariant: there may still be a lingering net_xmit, but it won't start a work queue, and it
    //            won't set priv->skb because is_shutdown is true.

    // free skb, in case it was abandoned by a cancelled work queue request
    if (priv->skb != NULL) {
        dev_consume_skb_any(priv->skb);
        priv->skb = NULL;
    }

    // remove the net device
    unregister_netdev(priv->ndev);

    // clear our local cache so that the index can be reused by subsequent rpmsg_probe
    rpmsg_net_device[priv->idx] = NULL;

    dev_info(&rpdev->dev, "removed channel: 0x%x -> 0x%x and unbound from interface rpmsg%d\n",
             rpdev->src, rpdev->dst, priv->idx);

    // cleanup
    free_netdev(priv->ndev);
}

////////////////////////
// Network Driver API //
////////////////////////

/** Network device driver operations object */
static const struct net_device_ops netdev_ops = {
    .ndo_open               = net_open,
    .ndo_stop               = net_close,
    .ndo_start_xmit         = net_xmit,
    .ndo_set_mac_address    = eth_mac_addr,
    .ndo_validate_addr      = eth_validate_addr,
    //FUTURE: .ndo_get_stats64    = sl_get_stats64,
    //FUTURE allow user to alter MTU within allowable range of RPMsg bus
};

/** Network device driver header operations object */
const struct header_ops header_ops = {
    .create                 = eth_header,
    .parse                  = eth_header_parse,
    .cache                  = eth_header_cache,
    .cache_update           = eth_header_cache_update,
};

/**
 * @brief Network setup callback. Called by the network stack when we register a new `rpmsgN`
 *        device. Job is to initialize the network device structure.
 *
 * @param ndev Network device to be initialized.
 */
static void net_setup(struct net_device *ndev)
{
    ndev->netdev_ops        = &netdev_ops;
    ndev->header_ops        = &header_ops;
    ndev->type              = ARPHRD_ETHER;
    ndev->hard_header_len   = RPMSG_NET_L2_HEADER_SIZE;
    ndev->min_header_len    = RPMSG_NET_L2_HEADER_SIZE;
    ndev->mtu               = RPMSG_NET_L2_MTU;
    //not available in 4.9: ndev->min_mtu           = ndev->mtu; //ETH_MIN_MTU;
    //not available in 4.9: ndev->max_mtu           = ndev->mtu; //ETH_DATA_LEN;
    ndev->addr_len          = ETH_ALEN;
    ndev->tx_queue_len      = 5; //DEFAULT_TX_QUEUE_LEN;
    // future: consider IFF_POINTTOPOINT and IFF_NOARP flags?
    //         is there a flag to signify that link is is always up?
    ndev->flags             = IFF_BROADCAST;
    ndev->priv_flags        |= IFF_TX_SKB_SHARING;

    // future: add NETIF_F_NO_CSUM feature to disable checksumming

    eth_broadcast_addr(ndev->broadcast);
}

/**
 * @brief Called when the network device is opened. Starts the transmission queue.
 *
 * @param ndev Network device to open
 *
 * @return 0 on success, -1 on error
 */
static int net_open(struct net_device *ndev)
{
    netif_start_queue(ndev);
    return 0;
}

/**
 * @brief Called when the network device is closed. Stops the transmission queue.
 *
 * @param ndev Network device to close
 *
 * @return 0 on success, -1 on error
 */
static int net_close(struct net_device *ndev)
{
    netif_stop_queue(ndev);
    return 0;
}

/**
 * @brief Called in workqueue context to re-try a previously failed transmission. Activates the
 *        `immediate` work queue to perform the actual work.
 *
 * @param work Work queue we were called on
 */
static void net_xmit_delayed_work_handler(struct work_struct *work)
{
    struct rpmsg_net_priv *priv = container_of(work, struct rpmsg_net_priv, delayed.work);
    unsigned long flags;

    spin_lock_irqsave(&priv->shutdown_lock, flags);
    if (priv->is_delayed && !priv->is_shutdown) {
        schedule_work(&priv->immediate);
        spin_unlock_irqrestore(&priv->shutdown_lock, flags);
    } else {
        spin_unlock_irqrestore(&priv->shutdown_lock, flags);

        dev_info(&priv->rpdev->dev, "delayed work handler skipping kick of immediate due to shutdown request\n");
    }
}

/**
 * @brief Called in workqueue context to attempt transmission of a network device skb. We attempt
 *        to send the packet via RPMsg bus to the other processor. On failure, we schedule one
 *        retransmission attempt in the future, before dropping the packet.
 *
 * @param work Work queue we were called on
 */
static void net_xmit_work_handler(struct work_struct *work)
{
    struct rpmsg_net_priv *priv = container_of(work, struct rpmsg_net_priv, immediate);
    unsigned long flags;
    int err;

    if (priv->skb == NULL) {
        dev_err(&priv->rpdev->dev, "net_xmit_work_handler called with NULL skb\n");
        goto cleanup;
    }

    err = rpmsg_trysend(priv->rpdev->ept, priv->skb->data, priv->skb->len);
    if (err) {
        if (priv->is_delayed) {
            // this is already our second attempt
            dev_err(&priv->rpdev->dev, "RPMsg send retry failed with error %d; dropping packet\n", err);

            // normal cleanup
            goto cleanup;
        } else {
            // first attempt failed; attempt retry if not shutdown
            spin_lock_irqsave(&priv->shutdown_lock, flags);
            if (!priv->is_shutdown) {
                priv->is_delayed = true;
                // Our goal is to sleep long enough to free at least one (1) packet in the RPMsg
                // ring buffer. When HZ is 100 (lowest setting), our minimum resolution is 10ms (1
                // jiffy). On Kestrel-M4, flood ping clocked in at ~600 1400-bytes packets per
                // second on an unloaded system, and ~200 packets/sec on a loaded system. So, 10ms
                // should give us at least one packet.
                schedule_delayed_work(&priv->delayed, (unsigned long)(0.5 + (0.010 * HZ)));
                spin_unlock_irqrestore(&priv->shutdown_lock, flags);

                dev_err(&priv->rpdev->dev, "RPMsg send failed with error %d; will retry\n", err);
            } else {
                spin_unlock_irqrestore(&priv->shutdown_lock, flags);

                dev_info(&priv->rpdev->dev, "skipping RPMsg send retry due to shutdown request\n");
            }

            // leave priv->skb populated, either for retry, or if under shutdown, to be freed by
            // rpmsg_remove()
            return;
        }
    }

    if (debug >= DBG_RX_TX) {
        dev_info(&priv->rpdev->dev, "sent packet on channel 0x%x -> 0x%x, len:%d\n",
                 priv->rpdev->src, priv->rpdev->dst, priv->skb->len);
    }

    if (debug >= DBG_PACKET) {
        // TODO build up packet debug message
    }

cleanup:
    // return the skb to the network stack
    if (priv->skb != NULL) {
        dev_consume_skb_any(priv->skb);
        priv->skb = NULL;
    }

    priv->is_delayed = false;

    // if not shutdown, re-activate the network stack xmit queue
    spin_lock_irqsave(&priv->shutdown_lock, flags);
    if (!priv->is_shutdown) {
        netif_wake_queue(priv->ndev);
    }
    spin_unlock_irqrestore(&priv->shutdown_lock, flags);
}

/**
 * @brief Called in ISR context by the network stack when the network device wants to transmit a
 *        frame. Our goal is to send this skb (frame) on the RPMsg stack. Since RPMsg send may block
 *        (sleep), we funnel this request over to the `immedate` workqueue, which runs in process
 *        context. Since RPMsg lacks a mechanism to poll the outbound queue state (we can't know if
 *        our next send will succeed, or fail), we always inhibit the net device's transmitter upon
 *        receipt of an skb. The workqueue handler will re-enable the net device's transmitter upon
 *        RPMsg send success (or eventual failure).
 *
 * @param skb  Frame to be sent
 * @param ndev Network device that originated the frame
 *
 * @return NETDEV_TX_OK on success, NETDEV_TX_BUSY on hard (unrecoverable) error
 */
static int net_xmit(struct sk_buff *skb, struct net_device *ndev)
{
    struct rpmsg_net_priv *priv = netdev_priv(ndev);
    unsigned long flags;

    if (debug >= DBG_INSANE) {
        dev_info(&priv->rpdev->dev, "net_xmit: in_interrupt:%lu [in_irq:%lu in_softirq:%lu] in_atomic:%u in_task:%u\n",
            in_interrupt(), in_irq(), in_softirq(), in_atomic(), in_task());
    }

    if (priv->skb != NULL) {
        // Hard (unrecoverable) error. When returning NETDEV_TX_BUSY, we must not keep a
        // reference to the skb, and we must not free it.

        dev_err(&priv->rpdev->dev, "net_xmit: error: previous skb still in progress! this can't happen.\n");
        return NETDEV_TX_BUSY;
    }

    // stop the net device transmitter. will be re-enabled by the work queue.
    netif_stop_queue(priv->ndev);

    // schedule work, unless we are shutdown
    spin_lock_irqsave(&priv->shutdown_lock, flags);
    if (!priv->is_shutdown) {
        priv->skb = skb;
        schedule_work(&priv->immediate);
        spin_unlock_irqrestore(&priv->shutdown_lock, flags);
    } else {
        // we're shut down. drop packet. leave queue stopped.
        spin_unlock_irqrestore(&priv->shutdown_lock, flags);

        dev_consume_skb_any(skb);
        dev_info(&priv->rpdev->dev, "net_xmit: dropping packet due to shutdown request (race)\n");
    }

    return NETDEV_TX_OK;
}

///////////////////////
// Kernel Module API //
//////////////////////

/**
 * @brief Initializes the rpmsg_net kernel module, registering it with the RPMsg stack.
 *
 * @return 0 on success, -1 on failure
 */
static int __init rpmsg_net_module_init(void)
{
    printk(KERN_INFO "rpmsg_net: module starting\n");

    // initialize global state
    memset(rpmsg_net_device, 0x00, sizeof(rpmsg_net_device));

    return register_rpmsg_driver(&rpmsg_net_client);
}

/**
 * @brief De-initializes the rpmsg_net kernel module, unregistering with the RPMsg stack. This,
 *        in turn, invokes the remove callback handler for all channels. Each remove callback will,
 *        in turn, unregister the associated netdevice from the network device stack.
 */
static void __exit rpmsg_net_module_exit(void)
{
    unregister_rpmsg_driver(&rpmsg_net_client);

    printk(KERN_INFO "rpmsg_net: module unloaded\n");
}

module_init(rpmsg_net_module_init);
module_exit(rpmsg_net_module_exit);
