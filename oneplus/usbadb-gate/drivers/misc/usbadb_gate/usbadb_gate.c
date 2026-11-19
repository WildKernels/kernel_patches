// SPDX-License-Identifier: GPL-2.0-only
/*
 * usbadb_gate - adb only while a non-dumb USB host (a PC) is attached.
 *
 * This driver only DETECTS and PUBLISHES. It runs nothing.
 *
 *   detect     /sys/class/udc/<udc>/state, delivered by the gadget state
 *              notifier (added by patches/0001). Only "configured" means a
 *              host sent SET_CONFIGURATION - a dumb charger never does, so it
 *              can never be configured. The state arrives in the event; it is
 *              not re-read on the decision path.
 *
 *   publish    /dev/usbadb_gate blocks until the verdict changes, then returns
 *              one line: "on configured 7".  /sys/kernel/usbadb_gate/link has
 *              the same verdict for one-shot reads.
 *
 *   act        someone else. patches/usbadb_gate.rc adds one trigger to
 *              KernelSU-Next's KERNEL_SU_RC, so init forks the reader itself:
 *
 *                  exec_background u:r:ksu:s0 root -- sh -c "..."
 *
 *              ...and it runs setprop plus start/stop adbd on each verdict.
 *
 *              Note the shape: one token, so init's tokenizer sees a single
 *              argument. It must be exec_background, not exec: exec sets
 *              SVC_EXEC and leaves Service::is_exec_service_running() true
 *              while the child lives, and this child never exits.
 *
 * Why not run the tools here: measured on a device. A child spawned from a
 * workqueue gets its tools executed (exit 0, even with SELinux permissive) and
 * init acts on neither the property write nor the service control message. The
 * same commands work from a caller init accepts. So the kernel decides and a
 * legitimate caller acts.
 *
 * Event driven throughout: the notifier is edge-triggered, and read() sleeps in
 * a wait queue. No polling, no periodic wakeup. There is one one-shot timer, it
 * exists only while a verdict is being held, and it is cancelled the moment the
 * cable comes back: writing sys.usb.config makes Android tear
 * the gadget down and re-enumerate it, so "not attached" arrives in the middle of
 * a re-enumeration too, and only elapsed time can tell that from a cable pull.
 *
 * This driver does not run, start or stop anything, and it does not read adb's
 * state. It publishes what the gadget reported; the reader decides and acts.
 *
 * Control: /sys/kernel/usbadb_gate/state
 *            1   enabled (default): judge the cable and publish verdicts
 *            0   disabled: publish nothing, the reader is left alone
 * Read:    /sys/kernel/usbadb_gate/link   "udc=%s src=%s enabled=%d hold=%d published=%s events=%lu"
 */
#define pr_fmt(fmt) "usbadb_gate: " fmt

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/workqueue.h>

/* folded by Kbuild from CONFIG_USBADB_GATE_UDC_STATE */
const char usbadb_gate_state_path[] = USBADB_GATE_STATE_PATH;

/* line length, including the trailing NUL: "off not attached 4294967295\n\0" */
#define USBADB_GATE_LINE 48

/*
 * The switch, and the only thing this driver can be told to do.
 *
 * The kernel cannot configure adb - it does not start services and it does not
 * write props; that is the reader's job, in init's context. So there is no
 * "force adb on" here to offer. There is only "is this driver judging the
 * cable".
 */
static int usbadb_gate_enabled = 1;

/* last published verdict, so only real changes are published */
static int usbadb_gate_applied = -1;

/* the decision input: delivered by the notifier, never read back */
static int usbadb_gate_gadget_state = -1;

/* trigger events seen; the first thing to look at when nothing happens */
static unsigned long usbadb_gate_events;

/*
 * How long a verdict has to hold before it is published, in either direction.
 *
 * The window has to outlast our own echo, and the echo is not the short part:
 *
 *   writing sys.usb.config makes Android's USB HAL tear the gadget down and
 *   bring it back up, and the port does not come back until the hub has
 *   re-enumerated it. On this device that is ~1.7 s from teardown to configured,
 *   and the reader's own adb_on()/adb_off() takes 0.1-0.9 s on top, so the
 *   reader is still working while the port is down.
 *
 * A window shorter than that publishes "off" in the middle of the echo, the
 * reader tears the port down again with its own mtp write, and the loop is back
 * with the settle window adding latency to it. 3000 ms leaves ~1.4 s of margin
 * over the measured round trip while still being well under the time it takes a
 * person to unplug a cable and plug it into another machine.
 *
 * The cost is that adb follows the cable 3 s late. That is the price of not
 * fighting ourselves; a shorter window is only safe for a device whose
 * re-enumeration is genuinely fast.
 */
#define USBADB_GATE_SETTLE_MS 3000

/* when the pending verdict was first seen, to time the settle window */
static unsigned long usbadb_gate_want_since;

/* the verdict being waited out: -1 none, 0 off, 1 on */
static int usbadb_gate_pending_on = -1;

/* /dev/usbadb_gate, the hand-off */
static DECLARE_WAIT_QUEUE_HEAD(usbadb_gate_wait);
static char usbadb_gate_line[USBADB_GATE_LINE] = "off unknown 0\n";
static size_t usbadb_gate_len = sizeof("off unknown 0\n") - 1;   /* without NUL */
static unsigned int usbadb_gate_seq;

/*
 * Read -> decide -> publish is one step in process context. A notifier callback
 * may only queue the work: it can hold locks that exec-free code must not block
 * under, and it must not block at all.
 */
static DEFINE_MUTEX(usbadb_gate_lock);
static struct work_struct usbadb_gate_work;
/* re-examines a held verdict when the settle window expires; not periodic */
static struct delayed_work usbadb_gate_settle;
static struct kobject *usbadb_gate_kobj;

static void usbadb_gate_queue(void)
{
    schedule_work(&usbadb_gate_work);
}

/*
 * Publish the verdict and wake the reader. Called with the lock held, so two
 * verdicts can never interleave in the buffer.
 */
static void usbadb_gate_publish(int on, const char *state)
{
    usbadb_gate_seq++;
    usbadb_gate_len = snprintf(usbadb_gate_line, sizeof(usbadb_gate_line),
                               "%s %s %u\n", on ? "on" : "off", state,
                               usbadb_gate_seq);
    wake_up_interruptible(&usbadb_gate_wait);
}

/* read the sysfs node, for the case where no event has arrived yet */
static int usbadb_gate_read_state(char *buf, size_t len)
{
    struct file *f;
    ssize_t n;

    f = filp_open(usbadb_gate_state_path, O_RDONLY, 0);
    if (IS_ERR(f))
        return PTR_ERR(f);

    n = kernel_read(f, buf, len - 1, &f->f_pos);
    filp_close(f, NULL);
    if (n < 0)
        return n;

    buf[n] = '\0';
    strim(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the decision                                                       */
/* ------------------------------------------------------------------ */

/*
 * Arm the settle timer for `on`, restarting the clock.
 *
 * schedule_delayed_work() does nothing when the work is already pending, and a
 * verdict that flips inside the window has to start its own window from the flip,
 * so the pending one is cancelled first. That is safe here: this runs in work()
 * context and the work being cancelled is not the one running (settle_fn only
 * queues work()).
 */
static void usbadb_gate_hold(int on, unsigned long now)
{
    usbadb_gate_pending_on = on;
    usbadb_gate_want_since = now;

    cancel_delayed_work_sync(&usbadb_gate_settle);
    schedule_delayed_work(&usbadb_gate_settle,
                          msecs_to_jiffies(USBADB_GATE_SETTLE_MS) + 1);
}

/* there is nothing left to publish: drop the pending verdict and its timer */
static void usbadb_gate_release(void)
{
    usbadb_gate_pending_on = -1;
    cancel_delayed_work_sync(&usbadb_gate_settle);
}

static void usbadb_gate_work_fn(struct work_struct *work)
{
    char state[32];
    unsigned long now = jiffies;
    bool configured;
    int on, err;

    (void)work;

    mutex_lock(&usbadb_gate_lock);

    if (usbadb_gate_gadget_state >= 0) {
        /* authoritative: the state came with the event */
        configured = usbadb_gate_gadget_state == USB_STATE_CONFIGURED;
        strscpy(state, usb_state_string(usbadb_gate_gadget_state), sizeof(state));
    } else {
        /*
         * No event yet (cable already in at boot). Best effort: this read is
         * refused in kernel context on some devices, and "refused" must mean
         * unknown - never "unplugged".
         */
        err = usbadb_gate_read_state(state, sizeof(state));
        if (err) {
            strscpy(state, "unknown", sizeof(state));
            pr_warn_once("%s unreadable here (%d), waiting for the first event\n",
                         usbadb_gate_state_path, err);
            configured = usbadb_gate_applied > 0;
        } else {
            configured = !strcmp(state, "configured");
        }
    }

    /*
     * Disabled: stop judging, and send nothing. The reader is told what is true
     * and nothing else; deciding what to do about it is not this driver's.
     */
    if (!usbadb_gate_enabled) {
        usbadb_gate_release();
        goto out;
    }

    on = configured;

    /* the verdict the reader would get right now; nothing to do if it has it */
    if (on == usbadb_gate_applied) {
        usbadb_gate_release();
        goto out;
    }

    /*
     * A verdict that differs from what the reader was told is held for the settle
     * window before it is published, in both directions.
     *
     * Time is the only thing that separates a real change from our own echo.
     * Writing sys.usb.config makes Android tear the gadget down and re-enumerate
     * it, so a phantom "not attached" and a real unplug arrive identically; and
     * the other way, an enumeration that drops again and a contact bouncing on
     * the connector look like a device arriving until it is still there later.
     *
     * This has to be a timer and not a count of events, because no event says
     * "the gadget has stopped changing". If the verdict flips meanwhile it starts
     * the window again from the flip; if it flips back to what the reader already
     * has, there is nothing to publish and the timer is cancelled.
     */
    if (usbadb_gate_pending_on != on) {
        usbadb_gate_hold(on, now);
        pr_info("udc=%s, holding %d ms before publishing %s\n",
                state, USBADB_GATE_SETTLE_MS, on ? "on" : "off");
        goto out;
    }
    if (time_before(now, usbadb_gate_want_since +
                         msecs_to_jiffies(USBADB_GATE_SETTLE_MS)))
        goto out;                          /* still inside the window */

    /* it held: publish it */
    usbadb_gate_release();

    usbadb_gate_applied = on;
    /* wake the reader; it is the only one that can make it happen */
    usbadb_gate_publish(on, state);
    /*
     * The driver has no idea whether adb is running: it never looks. Name the
     * verdict, not the consequence - the reader is what turns "on" into a started
     * daemon, and only the reader knows if that worked.
     */
    pr_info("publish %s: udc=%s\n", on ? "on" : "off", state);

out:
    mutex_unlock(&usbadb_gate_lock);
}

/* the settle window expiring is itself a reason to look again */
static void usbadb_gate_settle_fn(struct work_struct *work)
{
    (void)work;
    usbadb_gate_queue();
}

/* ------------------------------------------------------------------ */
/* trigger: the gadget state notifier                                 */
/* ------------------------------------------------------------------ */

static int usbadb_gate_udc_notify(struct notifier_block *nb,
                                  unsigned long state, void *data)
{
    (void)nb;
    (void)data;

    WRITE_ONCE(usbadb_gate_gadget_state, (int)state);
    usbadb_gate_events++;
    usbadb_gate_queue();

    return NOTIFY_OK;
}

static struct notifier_block usbadb_gate_udc_nb = {
    .notifier_call = usbadb_gate_udc_notify,
};

/* ------------------------------------------------------------------ */
/* /sys/kernel/usbadb_gate/{state,link}                               */
/* ------------------------------------------------------------------ */

static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr,
                          char *buf)
{
    (void)kobj;
    (void)attr;

    return sysfs_emit(buf, "%d\n", usbadb_gate_enabled);
}

static ssize_t state_store(struct kobject *kobj, struct kobj_attribute *attr,
                           const char *buf, size_t count)
{
    (void)kobj;
    (void)attr;

    if (sysfs_streq(buf, "1"))
        usbadb_gate_enabled = 1;
    else if (sysfs_streq(buf, "0"))
        usbadb_gate_enabled = 0;
    else
        return -EINVAL;

    /*
     * Re-enabling has to re-publish, because the cable may have moved while
     * nothing was watching: applied = -1 means "we do not know what the reader
     * believes", so the next verdict is published even if it did not change.
     */
    if (usbadb_gate_enabled)
        usbadb_gate_applied = -1;
    usbadb_gate_release();
    usbadb_gate_queue();

    return count;
}

static ssize_t link_show(struct kobject *kobj, struct kobj_attribute *attr,
                         char *buf)
{
    char state[32];
    const char *src;

    (void)kobj;
    (void)attr;

    if (READ_ONCE(usbadb_gate_gadget_state) >= 0) {
        strscpy(state, usb_state_string(READ_ONCE(usbadb_gate_gadget_state)),
                sizeof(state));
        src = "event";
    } else if (!usbadb_gate_read_state(state, sizeof(state))) {
        src = "sysfs";
    } else {
        strscpy(state, "unknown", sizeof(state));
        src = "none";
    }

    /*
     * No "adb=" column: nothing here knows whether adbd is running, and a field
     * that claims to would be believed. What the driver does know is what the
     * gadget reported and what it last published.
     */
    return sysfs_emit(buf,
                      "udc=%s src=%s enabled=%d held=%s published=%s events=%lu\n",
                      state, src, usbadb_gate_enabled,
                      usbadb_gate_pending_on < 0 ? "none" :
                      usbadb_gate_pending_on ? "on" : "off",
                      usbadb_gate_applied < 0 ? "unset" :
                      usbadb_gate_applied ? "on" : "off",
                      READ_ONCE(usbadb_gate_events));
}

static struct kobj_attribute usbadb_gate_state_attr =
    __ATTR(state, 0644, state_show, state_store);
static struct kobj_attribute usbadb_gate_link_attr = __ATTR_RO(link);

static struct attribute *usbadb_gate_attrs[] = {
    &usbadb_gate_state_attr.attr,
    &usbadb_gate_link_attr.attr,
    NULL,
};

static const struct attribute_group usbadb_gate_attr_group = {
    .attrs = usbadb_gate_attrs,
};

/* ------------------------------------------------------------------ */
/* /dev/usbadb_gate: one line per verdict, blocking read              */
/* ------------------------------------------------------------------ */

/*
 * Per-open state. This must NOT live in f->f_pos.
 *
 * vfs_read() hands read() a *copy* of f_pos and, because our file is seekable
 * (nonseekable_open only clears FMODE_LSEEK|PREAD|PWRITE; FMODE_ATOMIC_POS stays
 * set), ksys_read() writes that copy back over ours when we return:
 *
 *     loff_t pos, *ppos = file_ppos(fd_file(f));
 *     if (ppos) { pos = *ppos; ppos = &pos; }
 *     ret = vfs_read(fd_file(f), buf, count, ppos);
 *     if (ret >= 0 && ppos) fd_file(f)->f_pos = pos;      <-- clobbers us
 *
 * So advancing f_pos inside read() is silently undone, and every read restarts
 * at the same byte. That is exactly what the device showed: `cat` re-read the
 * same line until killed, and a shell read() asking for one byte at a time got
 * the same first character forever and never found its newline - so the reader
 * never returned from read() at all.
 *
 * The cursor therefore lives here, one per open, so two readers stay independent;
 * f_pos is not used at all, not even as an initial value.
 */
struct usbadb_gate_file {
    struct mutex lock;      /* protects next/off for this fd */
    unsigned int next;      /* sequence number of the line being delivered */
    size_t off;             /* bytes of it already delivered */
};

/* would read() return bytes right now, or block? */
static bool usbadb_gate_ready(struct usbadb_gate_file *st)
{
    unsigned int seq = READ_ONCE(usbadb_gate_seq);

    if (st->next < seq)
        return true;                            /* behind: latest wins */
    if (st->next > seq)
        return false;                           /* caught up */
    return st->off < READ_ONCE(usbadb_gate_len);
}

static ssize_t usbadb_gate_dev_read(struct file *f, char __user *ubuf,
                                    size_t len, loff_t *ppos)
{
    struct usbadb_gate_file *st = f->private_data;
    unsigned int seq;
    size_t have, n;
    ssize_t ret;

    (void)ppos;                    /* not a cursor: see struct usbadb_gate_file */

    if (!len)
        return 0;

    for (;;) {
        if (usbadb_gate_ready(st))
            break;
        if (f->f_flags & O_NONBLOCK)
            return -EAGAIN;
        ret = wait_event_interruptible(usbadb_gate_wait, usbadb_gate_ready(st));
        if (ret)
            return ret;            /* a signal is not an event */
    }

    mutex_lock(&usbadb_gate_lock);
    mutex_lock(&st->lock);

    seq = READ_ONCE(usbadb_gate_seq);

    if (st->next < seq) {
        /* the line this fd was part-way through has been replaced, so a
         * half-delivered line is abandoned rather than spliced onto a new one */
        st->next = seq;
        st->off = 0;
    }
    if (st->next > seq) {
        mutex_unlock(&st->lock);
        mutex_unlock(&usbadb_gate_lock);
        return -EAGAIN;
    }

    have = READ_ONCE(usbadb_gate_len);
    if (st->off > have)            /* the line got shorter under us */
        st->off = 0;

    n = min(len, have - st->off);
    if (n && copy_to_user(ubuf, usbadb_gate_line + st->off, n)) {
        mutex_unlock(&st->lock);
        mutex_unlock(&usbadb_gate_lock);
        return -EFAULT;
    }

    st->off += n;
    if (st->off >= have) {         /* this line is fully delivered */
        st->next++;
        st->off = 0;
    }

    mutex_unlock(&st->lock);
    mutex_unlock(&usbadb_gate_lock);

    return n;
}

static __poll_t usbadb_gate_dev_poll(struct file *f, poll_table *wait)
{
    poll_wait(f, &usbadb_gate_wait, wait);

    return usbadb_gate_ready(f->private_data) ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static int usbadb_gate_dev_open(struct inode *inode, struct file *f)
{
    struct usbadb_gate_file *st;

    st = kzalloc(sizeof(*st), GFP_KERNEL);
    if (!st)
        return -ENOMEM;
    mutex_init(&st->lock);

    /*
     * Start AT the current verdict, not after it: the first read returns what is
     * true right now, so the rc can reconcile before it waits for a change. A
     * reader that opened after the boot verdict was published and then blocked
     * would never act on it, which is precisely the state the device was left
     * in. Sequence numbers only prevent replay; they are not a subscription.
     */
    st->next = READ_ONCE(usbadb_gate_seq);
    st->off = 0;
    f->private_data = st;

    /*
     * This single log line answers the question nothing else could: whether the
     * rc block's child ever reached the device at all. "the driver published a
     * verdict" and "someone was listening" are different facts, and only this
     * one tells them apart.
     */
    pr_info("/dev/usbadb_gate opened by pid %d (%.16s)\n",
            task_pid_nr(current), current->comm);

    /*
     * nonseekable_open() is still right - it refuses lseek/pread/pwrite - but it
     * is NOT what keeps f_pos out of our way; see the note on
     * struct usbadb_gate_file. All our cursor state is in private_data.
     */
    return nonseekable_open(inode, f);
}

static int usbadb_gate_dev_release(struct inode *inode, struct file *f)
{
    struct usbadb_gate_file *st = f->private_data;

    f->private_data = NULL;
    kfree(st);
    return 0;
}

static const struct file_operations usbadb_gate_dev_fops = {
    .owner  = THIS_MODULE,
    .open   = usbadb_gate_dev_open,
    .read   = usbadb_gate_dev_read,
    .poll   = usbadb_gate_dev_poll,
    .release = usbadb_gate_dev_release,
    /* no .llseek: usbadb_gate_dev_open() calls nonseekable_open() */
};

static struct miscdevice usbadb_gate_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "usbadb_gate",
    .fops  = &usbadb_gate_dev_fops,
    .mode  = 0600, /* root only; the reader runs as root */
};

/* ------------------------------------------------------------------ */
/* init                                                               */
/* ------------------------------------------------------------------ */

/*
 * late_initcall, not module_init: the gadget may already be up, and we want any
 * stray event to have been registered for before the first decision.
 */
static int __init usbadb_gate_init(void)
{
    int ret;

    INIT_WORK(&usbadb_gate_work, usbadb_gate_work_fn);
    INIT_DELAYED_WORK(&usbadb_gate_settle, usbadb_gate_settle_fn);

    usbadb_gate_kobj = kobject_create_and_add("usbadb_gate", kernel_kobj);
    if (!usbadb_gate_kobj) {
        pr_err("cannot create /sys/kernel/usbadb_gate\n");
        return -ENOMEM;
    }

    ret = sysfs_create_group(usbadb_gate_kobj, &usbadb_gate_attr_group);
    if (ret) {
        pr_err("cannot create sysfs attributes: %d\n", ret);
        goto err_kobj;
    }

    ret = misc_register(&usbadb_gate_misc);
    if (ret) {
        pr_err("cannot register /dev/usbadb_gate: %d\n", ret);
        goto err_sysfs;
    }

    ret = usb_gadget_register_state_notifier(&usbadb_gate_udc_nb);
    if (ret) {
        pr_err("cannot register udc state notifier: %d\n", ret);
        goto err_misc;
    }

    pr_info("detect only: the verdict is published, patches/usbadb_gate.rc acts\n");
    pr_info("watch: gadget state notifier, edge triggered; no polling, no timers\n");
    pr_info("hand-off: /dev/usbadb_gate blocks until the verdict changes\n");
    pr_info("read: /sys/kernel/usbadb_gate/link, control: .../state\n");

    /* the first decision, now that everything is registered */
    usbadb_gate_queue();

    return 0;

err_misc:
    misc_deregister(&usbadb_gate_misc);
err_sysfs:
    sysfs_remove_group(usbadb_gate_kobj, &usbadb_gate_attr_group);
err_kobj:
    kobject_put(usbadb_gate_kobj);
    return ret;
}

/*
 * Test-only entry points, for tests/read_pos_check.py - the one harness that
 * drives read() through a kernel-shaped file_operations table instead of a FIFO.
 *
 * USBADB_GATE_TEST is never defined by the real build: Kconfig has no such
 * symbol, so none of this reaches a shipping kernel. It exists because the
 * f_pos bug was invisible to every offline test that did not model ksys_read().
 */
#ifdef USBADB_GATE_TEST

void usbadb_gate_test_init(void)
{
    mutex_init(&usbadb_gate_lock);
}

const struct file_operations *usbadb_gate_test_fops(void)
{
    return &usbadb_gate_dev_fops;
}

void usbadb_gate_test_publish(int on, const char *state)
{
    mutex_lock(&usbadb_gate_lock);
    usbadb_gate_publish(on, state);
    mutex_unlock(&usbadb_gate_lock);
}

void usbadb_gate_test_exit(void)
{
}

#endif /* USBADB_GATE_TEST */

/*
 * Built in, which is the normal case here, there is nothing to unload and no
 * exit to register. Guarded rather than unconditional: an unconditional
 * module_exit() makes the compiler parse a macro the build may not provide.
 */
#ifdef MODULE
static void __exit usbadb_gate_exit(void)
{
    usb_gadget_unregister_state_notifier(&usbadb_gate_udc_nb);
    cancel_work_sync(&usbadb_gate_work);

    misc_deregister(&usbadb_gate_misc);
    sysfs_remove_group(usbadb_gate_kobj, &usbadb_gate_attr_group);
    kobject_put(usbadb_gate_kobj);

    pr_info("removed\n");
}
module_exit(usbadb_gate_exit);
#endif

late_initcall(usbadb_gate_init);
