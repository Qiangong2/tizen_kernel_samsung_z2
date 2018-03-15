/*
 *  Dynamic Binary Instrumentation Module based on KProbes
 *  modules/energy/swap_energy.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) Samsung Electronics, 2013
 *
 * 2013         Vasiliy Ulyanov <v.ulyanov@samsung.com>
 *              Vyacheslav Cherkashin <v.cherkashin@samsung.com>
 *
 */


#include <linux/module.h>
#include <linux/file.h>
#include <linux/spinlock.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/fdtable.h>
#include <net/sock.h>
#include <kprobe/swap_kprobes.h>
#include <ksyms/ksyms.h>
#include <master/swap_deps.h>
#include <us_manager/sspt/sspt_proc.h>
#include <us_manager/sspt/sspt_feature.h>
#include <linux/atomic.h>
#include "energy.h"
#include "lcd/lcd_base.h"
#include "tm_stat.h"


/* ============================================================================
 * =                              ENERGY_XXX                                  =
 * ============================================================================
 */
struct kern_probe {
	const char *name;
	struct kretprobe *rp;
};

static int energy_xxx_once(struct kern_probe p[], int size)
{
	int i;
	const char *sym;

	for (i = 0; i < size; ++i) {
		struct kretprobe *rp = p[i].rp;

		sym = p[i].name;
		rp->kp.addr = (kprobe_opcode_t *)swap_ksyms(sym);
		if (rp->kp.addr == NULL)
			goto not_found;
	}

	return 0;

not_found:
	printk(KERN_INFO "ERROR: symbol '%s' not found\n", sym);
	return -ESRCH;
}

static int energy_xxx_set(struct kern_probe p[], int size, int *flag)
{
	int i, ret;

	for (i = 0; i < size; ++i) {
		ret = swap_register_kretprobe(p[i].rp);
		if (ret)
			goto fail;
	}

	*flag = 1;
	return 0;

fail:
	pr_err("swap_register_kretprobe(%s) ret=%d\n", p[i].name, ret);

	for (--i; i != -1; --i)
		swap_unregister_kretprobe(p[i].rp);

	return ret;
}

static void energy_xxx_unset(struct kern_probe p[], int size, int *flag)
{
	int i;

	if (*flag == 0)
		return;

	for (i = size - 1; i != -1; --i)
		swap_unregister_kretprobe(p[i].rp);

	*flag = 0;
}





/* ============================================================================
 * =                              CPUS_TIME                                   =
 * ============================================================================
 */
struct cpus_time {
	spinlock_t lock; /* for concurrent access */
	struct tm_stat tm[NR_CPUS];
};

#define cpus_time_lock(ct, flags) spin_lock_irqsave(&(ct)->lock, flags)
#define cpus_time_unlock(ct, flags) spin_unlock_irqrestore(&(ct)->lock, flags)

static void cpus_time_init(struct cpus_time *ct, u64 time)
{
	int cpu;

	spin_lock_init(&ct->lock);

	for (cpu = 0; cpu < NR_CPUS; ++cpu) {
		tm_stat_init(&ct->tm[cpu]);
		tm_stat_set_timestamp(&ct->tm[cpu], time);
	}
}

static inline u64 cpu_time_get_running(struct cpus_time *ct, int cpu, u64 now)
{
	return tm_stat_current_running(&ct->tm[cpu], now);
}

static void *cpus_time_get_running_all(struct cpus_time *ct, u64 *buf, u64 now)
{
	int cpu;

	for (cpu = 0; cpu < NR_CPUS; ++cpu)
		buf[cpu] = tm_stat_current_running(&ct->tm[cpu], now);

	return buf;
}

static void *cpus_time_sum_running_all(struct cpus_time *ct, u64 *buf, u64 now)
{
	int cpu;

	for (cpu = 0; cpu < NR_CPUS; ++cpu)
		buf[cpu] += tm_stat_current_running(&ct->tm[cpu], now);

	return buf;
}

static void cpus_time_save_entry(struct cpus_time *ct, int cpu, u64 time)
{
	struct tm_stat *tm = &ct->tm[cpu];

	if (unlikely(tm_stat_timestamp(tm))) /* should never happen */
		printk(KERN_INFO "XXX %s[%d/%d]: WARNING tmstamp(%p) set on cpu(%d)\n",
		       current->comm, current->tgid, current->pid, tm, cpu);
	tm_stat_set_timestamp(&ct->tm[cpu], time);
}

static void cpus_time_update_running(struct cpus_time *ct, int cpu, u64 now,
				     u64 start_time)
{
	struct tm_stat *tm = &ct->tm[cpu];

	if (unlikely(tm_stat_timestamp(tm) == 0)) {
		/* not initialized. should happen only once per cpu/task */
		printk(KERN_INFO "XXX %s[%d/%d]: nnitializing tmstamp(%p) "
		       "on cpu(%d)\n",
		       current->comm, current->tgid, current->pid, tm, cpu);
		tm_stat_set_timestamp(tm, start_time);
	}

	tm_stat_update(tm, now);
	tm_stat_set_timestamp(tm, 0); /* set timestamp to 0 */
}





struct energy_data {
	/* for __switch_to */
	struct cpus_time ct;

	/* for sys_read */
	atomic64_t bytes_read;

	/*for sys_write */
	atomic64_t bytes_written;

	/*for recvmsg*/
	atomic64_t bytes_recv;

	/* for sock_send */
	atomic64_t bytes_send;

	/* for l2cap_recv */
	atomic64_t bytes_l2cap_recv_acldata;

	/* for sco_recv_scodata */
	atomic64_t bytes_sco_recv_scodata;

	/* for hci_send_acl */
	atomic64_t bytes_hci_send_acl;

	/* for hci_send_sco */
	atomic64_t bytes_hci_send_sco;
};

static sspt_feature_id_t feature_id = SSPT_FEATURE_ID_BAD;

static void init_ed(struct energy_data *ed)
{
	/* instead of get_ntime(), CPU time is initialized to 0 here. Timestamp
	 * value will be properly set when the corresponding __switch_to event
	 * occurs */
	cpus_time_init(&ed->ct, 0);
	atomic64_set(&ed->bytes_read, 0);
	atomic64_set(&ed->bytes_written, 0);
	atomic64_set(&ed->bytes_recv, 0);
	atomic64_set(&ed->bytes_send, 0);
	atomic64_set(&ed->bytes_l2cap_recv_acldata, 0);
	atomic64_set(&ed->bytes_sco_recv_scodata, 0);
	atomic64_set(&ed->bytes_hci_send_acl, 0);
	atomic64_set(&ed->bytes_hci_send_sco, 0);
}

static void uninit_ed(struct energy_data *ed)
{
	cpus_time_init(&ed->ct, 0);
	atomic64_set(&ed->bytes_read, 0);
	atomic64_set(&ed->bytes_written, 0);
	atomic64_set(&ed->bytes_recv, 0);
	atomic64_set(&ed->bytes_send, 0);
	atomic64_set(&ed->bytes_l2cap_recv_acldata, 0);
	atomic64_set(&ed->bytes_sco_recv_scodata, 0);
	atomic64_set(&ed->bytes_hci_send_acl, 0);
	atomic64_set(&ed->bytes_hci_send_sco, 0);
}

static void *create_ed(void)
{
	struct energy_data *ed;

	ed = kmalloc(sizeof(*ed), GFP_ATOMIC);
	if (ed)
		init_ed(ed);

	return (void *)ed;
}

static void destroy_ed(void *data)
{
	struct energy_data *ed = (struct energy_data *)data;
	kfree(ed);
}


static int init_feature(void)
{
	feature_id = sspt_register_feature(create_ed, destroy_ed);

	if (feature_id == SSPT_FEATURE_ID_BAD)
		return -EPERM;

	return 0;
}

static void uninit_feature(void)
{
	sspt_unregister_feature(feature_id);
	feature_id = SSPT_FEATURE_ID_BAD;
}

static struct energy_data *get_energy_data(struct task_struct *task)
{
	void *data = NULL;
	struct sspt_proc *proc;

	proc = sspt_proc_get_by_task(task);
	if (proc)
		data = sspt_get_feature_data(proc->feature, feature_id);

	return (struct energy_data *)data;
}

static int check_fs(unsigned long magic)
{
	switch (magic) {
	case EXT2_SUPER_MAGIC: /* == EXT3_SUPER_MAGIC == EXT4_SUPER_MAGIC */
	case MSDOS_SUPER_MAGIC:
		return 1;
	}

	return 0;
}

static int check_ftype(int fd)
{
	int err, ret = 0;
	struct kstat kstat;

	err = vfs_fstat(fd, &kstat);
	if (err == 0 && S_ISREG(kstat.mode))
		ret = 1;

	return ret;
}

static int check_file(int fd)
{
	struct file *file;

	file = fget(fd);
	if (file) {
		int magic = 0;
		if (file->f_dentry && file->f_dentry->d_sb)
			magic = file->f_dentry->d_sb->s_magic;

		fput(file);

		if (check_fs(magic) && check_ftype(fd))
			return 1;
	}

	return 0;
}

static unsigned long get_arg0(struct pt_regs *regs)
{
#if defined(CONFIG_ARM)
	return regs->ARM_r0;
#elif defined(CONFIG_X86_32)
	return regs->bx;
#else
	#error "this architecture is not supported"
#endif /* CONFIG_arch */
}





static struct cpus_time ct_idle;
static struct energy_data ed_system;
static u64 start_time;

static void init_data_energy(void)
{
	start_time = get_ntime();
	init_ed(&ed_system);
	cpus_time_init(&ct_idle, 0);
}

static void uninit_data_energy(void)
{
	start_time = 0;
	uninit_ed(&ed_system);
	cpus_time_init(&ct_idle, 0);
}





/* ============================================================================
 * =                             __switch_to                                  =
 * ============================================================================
 */
static int entry_handler_switch(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	int cpu;
	struct cpus_time *ct;
	struct energy_data *ed;
	unsigned long flags;

	cpu = smp_processor_id();

	ct = current->tgid ? &ed_system.ct : &ct_idle;
	cpus_time_lock(ct, flags);
	cpus_time_update_running(ct, cpu, get_ntime(), start_time);
	cpus_time_unlock(ct, flags);

	ed = get_energy_data(current);
	if (ed) {
		ct = &ed->ct;
		cpus_time_lock(ct, flags);
		cpus_time_update_running(ct, cpu, get_ntime(), start_time);
		cpus_time_unlock(ct, flags);
	}

	return 0;
}

static int ret_handler_switch(struct kretprobe_instance *ri,
			      struct pt_regs *regs)
{
	int cpu;
	struct cpus_time *ct;
	struct energy_data *ed;
	unsigned long flags;

	cpu = smp_processor_id();

	ct = current->tgid ? &ed_system.ct : &ct_idle;
	cpus_time_lock(ct, flags);
	cpus_time_save_entry(ct, cpu, get_ntime());
	cpus_time_unlock(ct, flags);

	ed = get_energy_data(current);
	if (ed) {
		ct = &ed->ct;
		cpus_time_lock(ct, flags);
		cpus_time_save_entry(ct, cpu, get_ntime());
		cpus_time_unlock(ct, flags);
	}

	return 0;
}

static struct kretprobe switch_to_krp = {
	.entry_handler = entry_handler_switch,
	.handler = ret_handler_switch,
};





/* ============================================================================
 * =                                sys_read                                  =
 * ============================================================================
 */
struct sys_read_data {
	int fd;
};

static int entry_handler_sys_read(struct kretprobe_instance *ri,
				  struct pt_regs *regs)
{
	struct sys_read_data *srd = (struct sys_read_data *)ri->data;

	srd->fd = (int)get_arg0(regs);

	return 0;
}

static int ret_handler_sys_read(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	int ret = regs_return_value(regs);

	if (ret > 0) {
		struct sys_read_data *srd;

		srd = (struct sys_read_data *)ri->data;
		if (check_file(srd->fd)) {
			struct energy_data *ed;

			ed = get_energy_data(current);
			if (ed)
				atomic64_add(ret, &ed->bytes_read);

			atomic64_add(ret, &ed_system.bytes_read);
		}
	}

	return 0;
}

static struct kretprobe sys_read_krp = {
	.entry_handler = entry_handler_sys_read,
	.handler = ret_handler_sys_read,
	.data_size = sizeof(struct sys_read_data)
};





/* ============================================================================
 * =                               sys_write                                  =
 * ============================================================================
 */
static int entry_handler_sys_write(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	struct sys_read_data *srd = (struct sys_read_data *)ri->data;

	srd->fd = (int)get_arg0(regs);

	return 0;
}

static int ret_handler_sys_write(struct kretprobe_instance *ri,
				 struct pt_regs *regs)
{
	int ret = regs_return_value(regs);

	if (ret > 0) {
		struct sys_read_data *srd;

		srd = (struct sys_read_data *)ri->data;
		if (check_file(srd->fd)) {
			struct energy_data *ed;

			ed = get_energy_data(current);
			if (ed)
				atomic64_add(ret, &ed->bytes_written);

			atomic64_add(ret, &ed_system.bytes_written);
		}
	}

	return 0;
}

static struct kretprobe sys_write_krp = {
	.entry_handler = entry_handler_sys_write,
	.handler = ret_handler_sys_write,
	.data_size = sizeof(struct sys_read_data)
};





/* ============================================================================
 * =                                wifi                                      =
 * ============================================================================
 */
static bool check_wlan0(struct socket *sock)
{
	/* FIXME: hardcode interface */
	const char *name_intrf = "wlan0";

	if (sock->sk->sk_dst_cache &&
	    sock->sk->sk_dst_cache->dev &&
	    !strcmp(sock->sk->sk_dst_cache->dev->name, name_intrf))
		return true;

	return false;
}

static bool check_socket(struct task_struct *task, struct socket *socket)
{
	bool ret = false;
	unsigned int fd;
	struct files_struct *files;

	files = swap_get_files_struct(task);
	if (files == NULL)
		return false;

	rcu_read_lock();
	for (fd = 0; fd < files_fdtable(files)->max_fds; ++fd) {
		if (fcheck_files(files, fd) == socket->file) {
			ret = true;
			goto unlock;
		}
	}

unlock:
	rcu_read_unlock();
	swap_put_files_struct(files);
	return ret;
}

static struct energy_data *get_energy_data_by_socket(struct task_struct *task,
						     struct socket *socket)
{
	struct energy_data *ed;

	ed = get_energy_data(task);
	if (ed)
		ed = check_socket(task, socket) ? ed : NULL;

	return ed;
}

static int wf_sock_eh(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct socket *socket = (struct socket *)swap_get_karg(regs, 0);

	*(struct socket **)ri->data = check_wlan0(socket) ? socket : NULL;

	return 0;
}

static int wf_sock_aio_eh(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct kiocb *iocb = (struct kiocb *)swap_get_karg(regs, 0);
	struct socket *socket = iocb->ki_filp->private_data;

	*(struct socket **)ri->data = check_wlan0(socket) ? socket : NULL;

	return 0;
}

static int wf_sock_recv_rh(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	int ret = regs_return_value(regs);

	if (ret > 0) {
		struct socket *socket = *(struct socket **)ri->data;

		if (socket) {
			struct energy_data *ed;

			ed = get_energy_data_by_socket(current, socket);
			if (ed)
				atomic64_add(ret, &ed->bytes_recv);
			atomic64_add(ret, &ed_system.bytes_recv);
		}
	}

	return 0;
}

static int wf_sock_send_rh(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	int ret = regs_return_value(regs);

	if (ret > 0) {
		struct socket *socket = *(struct socket **)ri->data;

		if (socket) {
			struct energy_data *ed;

			ed = get_energy_data_by_socket(current, socket);
			if (ed)
				atomic64_add(ret, &ed->bytes_send);
			atomic64_add(ret, &ed_system.bytes_send);
		}
	}

	return 0;
}

static struct kretprobe sock_recv_krp = {
	.entry_handler = wf_sock_eh,
	.handler = wf_sock_recv_rh,
	.data_size = sizeof(struct socket *)
};

static struct kretprobe sock_send_krp = {
	.entry_handler = wf_sock_eh,
	.handler = wf_sock_send_rh,
	.data_size = sizeof(struct socket *)
};

static struct kretprobe sock_aio_read_krp = {
	.entry_handler = wf_sock_aio_eh,
	.handler = wf_sock_recv_rh,
	.data_size = sizeof(struct socket *)
};

static struct kretprobe sock_aio_write_krp = {
	.entry_handler = wf_sock_aio_eh,
	.handler = wf_sock_send_rh,
	.data_size = sizeof(struct socket *)
};

static struct kern_probe wifi_probes[] = {
	{
		.name = "sock_recvmsg",
		.rp = &sock_recv_krp,
	},
	{
		.name = "sock_sendmsg",
		.rp = &sock_send_krp,
	},
	{
		.name = "sock_aio_read",
		.rp = &sock_aio_read_krp,
	},
	{
		.name = "sock_aio_write",
		.rp = &sock_aio_write_krp,
	}
};

enum { wifi_probes_cnt = ARRAY_SIZE(wifi_probes) };
static int wifi_flag = 0;





/* ============================================================================
 * =                                bluetooth                                 =
 * ============================================================================
 */

struct swap_bt_data {
	struct socket *socket;
};

static int bt_entry_handler(struct kretprobe_instance *ri,
			    struct pt_regs *regs)
{
	struct swap_bt_data *data = (struct swap_bt_data *)ri->data;
	struct socket *sock = (struct socket *)swap_get_sarg(regs, 1);

	data->socket = sock ? sock : NULL;

	return 0;
}

static int bt_recvmsg_handler(struct kretprobe_instance *ri,
			      struct pt_regs *regs)
{
	int ret = regs_return_value(regs);
	struct swap_bt_data *data = (struct swap_bt_data *)ri->data;

	if (ret > 0) {
		struct socket *socket = data->socket;

		if (socket) {
			struct energy_data *ed;

			ed = get_energy_data_by_socket(current, socket);
			if (ed)
				atomic64_add(ret, &ed->bytes_l2cap_recv_acldata);
		}
		atomic64_add(ret, &ed_system.bytes_l2cap_recv_acldata);
	}

	return 0;
}

static int bt_sendmsg_handler(struct kretprobe_instance *ri,
			      struct pt_regs *regs)
{
	int ret = regs_return_value(regs);
	struct swap_bt_data *data = (struct swap_bt_data *)ri->data;

	if (ret > 0) {
		struct socket *socket = data->socket;

		if (socket) {
			struct energy_data *ed;

			ed = get_energy_data_by_socket(current, socket);
			if (ed)
				atomic64_add(ret, &ed->bytes_hci_send_sco);
		}
		atomic64_add(ret, &ed_system.bytes_hci_send_sco);
	}

	return 0;
}

static struct kretprobe rfcomm_sock_recvmsg_krp = {
	.entry_handler = bt_entry_handler,
	.handler = bt_recvmsg_handler,
	.data_size = sizeof(struct swap_bt_data)
};

static struct kretprobe l2cap_sock_recvmsg_krp = {
	.entry_handler = bt_entry_handler,
	.handler = bt_recvmsg_handler,
	.data_size = sizeof(struct swap_bt_data)
};

static struct kretprobe hci_sock_recvmsg_krp = {
	.entry_handler = bt_entry_handler,
	.handler = bt_recvmsg_handler,
	.data_size = sizeof(struct swap_bt_data)
};

static struct kretprobe sco_sock_recvmsg_krp = {
	.entry_handler = bt_entry_handler,
	.handler = bt_recvmsg_handler,
	.data_size = sizeof(struct swap_bt_data)
};
static struct kretprobe rfcomm_sock_sendmsg_krp = {
	.entry_handler = bt_entry_handler,
	.handler = bt_sendmsg_handler,
	.data_size = sizeof(struct swap_bt_data)
};

static struct kretprobe l2cap_sock_sendmsg_krp = {
	.entry_handler = bt_entry_handler,
	.handler = bt_sendmsg_handler,
	.data_size = sizeof(struct swap_bt_data)
};

static struct kretprobe hci_sock_sendmsg_krp = {
	.entry_handler = bt_entry_handler,
	.handler = bt_sendmsg_handler,
	.data_size = sizeof(struct swap_bt_data)
};

static struct kretprobe sco_sock_sendmsg_krp = {
	.entry_handler = bt_entry_handler,
	.handler = bt_sendmsg_handler,
	.data_size = sizeof(struct swap_bt_data)
};

static struct kern_probe bt_probes[] = {
	{
		.name = "rfcomm_sock_recvmsg",
		.rp = &rfcomm_sock_recvmsg_krp,
	},
	{
		.name = "l2cap_sock_recvmsg",
		.rp = &l2cap_sock_recvmsg_krp,
	},
	{
		.name = "hci_sock_recvmsg",
		.rp = &hci_sock_recvmsg_krp,
	},
	{
		.name = "sco_sock_recvmsg",
		.rp = &sco_sock_recvmsg_krp,
	},
	{
		.name = "rfcomm_sock_sendmsg",
		.rp = &rfcomm_sock_sendmsg_krp,
	},
	{
		.name = "l2cap_sock_sendmsg",
		.rp = &l2cap_sock_sendmsg_krp,
	},
	{
		.name = "hci_sock_sendmsg",
		.rp = &hci_sock_sendmsg_krp,
	},
	{
		.name = "sco_sock_sendmsg",
		.rp = &sco_sock_sendmsg_krp,
	}
};

enum { bt_probes_cnt = ARRAY_SIZE(bt_probes) };
static int energy_bt_flag = 0;

enum parameter_type {
	PT_CPU,
	PT_READ,
	PT_WRITE,
	PT_WF_RECV,
	PT_WF_SEND,
	PT_L2CAP_RECV,
	PT_SCO_RECV,
	PT_SEND_ACL,
	PT_SEND_SCO
};

struct cmd_pt {
	enum parameter_type pt;
	void *buf;
	int sz;
};

static void callback_for_proc(struct sspt_proc *proc, void *data)
{
	void *f_data = sspt_get_feature_data(proc->feature, feature_id);
	struct energy_data *ed = (struct energy_data *)f_data;

	if (ed) {
		unsigned long flags;
		struct cmd_pt *cmdp = (struct cmd_pt *)data;
		u64 *val = cmdp->buf;

		switch (cmdp->pt) {
		case PT_CPU:
			cpus_time_lock(&ed->ct, flags);
			cpus_time_sum_running_all(&ed->ct, val, get_ntime());
			cpus_time_unlock(&ed->ct, flags);
			break;
		case PT_READ:
			*val += atomic64_read(&ed->bytes_read);
			break;
		case PT_WRITE:
			*val += atomic64_read(&ed->bytes_written);
			break;
		case PT_WF_RECV:
			*val += atomic64_read(&ed->bytes_recv);
			break;
		case PT_WF_SEND:
			*val += atomic64_read(&ed->bytes_send);
			break;
		case PT_L2CAP_RECV:
			*val += atomic64_read(&ed->bytes_l2cap_recv_acldata);
			break;
		case PT_SCO_RECV:
			*val += atomic64_read(&ed->bytes_sco_recv_scodata);
			break;
		case PT_SEND_ACL:
			*val += atomic64_read(&ed->bytes_hci_send_acl);
			break;
		case PT_SEND_SCO:
			*val += atomic64_read(&ed->bytes_hci_send_sco);
			break;
		default:
			break;
		}
	}
}

static int current_parameter_apps(enum parameter_type pt, void *buf, int sz)
{
	struct cmd_pt cmdp;

	cmdp.pt = pt;
	cmdp.buf = buf;
	cmdp.sz = sz;

	on_each_proc(callback_for_proc, (void *)&cmdp);

	return 0;
}

/**
 * @brief Get energy parameter
 *
 * @param pe Type of energy parameter
 * @param buf Buffer
 * @param sz Buffer size
 * @return Error code
 */
int get_parameter_energy(enum parameter_energy pe, void *buf, size_t sz)
{
	unsigned long flags;
	u64 *val = buf; /* currently all parameters are u64 vals */
	int ret = 0;

	switch (pe) {
	case PE_TIME_IDLE:
		cpus_time_lock(&ct_idle, flags);
		/* for the moment we consider only CPU[0] idle time */
		*val = cpu_time_get_running(&ct_idle, 0, get_ntime());
		cpus_time_unlock(&ct_idle, flags);
		break;
	case PE_TIME_SYSTEM:
		cpus_time_lock(&ed_system.ct, flags);
		cpus_time_get_running_all(&ed_system.ct, val, get_ntime());
		cpus_time_unlock(&ed_system.ct, flags);
		break;
	case PE_TIME_APPS:
		current_parameter_apps(PT_CPU, buf, sz);
		break;
	case PE_READ_SYSTEM:
		*val = atomic64_read(&ed_system.bytes_read);
		break;
	case PE_WRITE_SYSTEM:
		*val = atomic64_read(&ed_system.bytes_written);
		break;
	case PE_WF_RECV_SYSTEM:
		*val = atomic64_read(&ed_system.bytes_recv);
		break;
	case PE_WF_SEND_SYSTEM:
		*val = atomic64_read(&ed_system.bytes_send);
		break;
	case PE_L2CAP_RECV_SYSTEM:
		*val = atomic64_read(&ed_system.bytes_l2cap_recv_acldata);
		break;
	case PE_SCO_RECV_SYSTEM:
		*val = atomic64_read(&ed_system.bytes_sco_recv_scodata);
		break;
	case PT_SEND_ACL_SYSTEM:
		*val = atomic64_read(&ed_system.bytes_hci_send_acl);
		break;
	case PT_SEND_SCO_SYSTEM:
		*val = atomic64_read(&ed_system.bytes_hci_send_sco);
		break;
	case PE_READ_APPS:
		current_parameter_apps(PT_READ, buf, sz);
		break;
	case PE_WRITE_APPS:
		current_parameter_apps(PT_WRITE, buf, sz);
		break;
	case PE_WF_RECV_APPS:
		current_parameter_apps(PT_WF_RECV, buf, sz);
		break;
	case PE_WF_SEND_APPS:
		current_parameter_apps(PT_WF_SEND, buf, sz);
		break;
	case PE_L2CAP_RECV_APPS:
		current_parameter_apps(PT_L2CAP_RECV, buf, sz);
		break;
	case PE_SCO_RECV_APPS:
		current_parameter_apps(PT_SCO_RECV, buf, sz);
		break;
	case PT_SEND_ACL_APPS:
		current_parameter_apps(PT_SEND_ACL, buf, sz);
		break;
	case PT_SEND_SCO_APPS:
		current_parameter_apps(PT_SEND_SCO, buf, sz);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

int do_set_energy(void)
{
	int ret = 0;

	init_data_energy();

	ret = swap_register_kretprobe(&sys_read_krp);
	if (ret) {
		printk(KERN_INFO "swap_register_kretprobe(sys_read) "
		       "result=%d!\n", ret);
		return ret;
	}

	ret = swap_register_kretprobe(&sys_write_krp);
	if (ret != 0) {
		printk(KERN_INFO "swap_register_kretprobe(sys_write) "
		       "result=%d!\n", ret);
		goto unregister_sys_read;
	}

	ret = swap_register_kretprobe(&switch_to_krp);
	if (ret) {
		printk(KERN_INFO "swap_register_kretprobe(__switch_to) "
		       "result=%d!\n",
		       ret);
		goto unregister_sys_write;
	}

	energy_xxx_set(bt_probes, bt_probes_cnt, &energy_bt_flag);
	energy_xxx_set(wifi_probes, wifi_probes_cnt, &wifi_flag);

	/* TODO: check return value */
	lcd_set_energy();

	return ret;

unregister_sys_read:
	swap_unregister_kretprobe(&sys_read_krp);

unregister_sys_write:
	swap_unregister_kretprobe(&sys_write_krp);

	return ret;
}

void do_unset_energy(void)
{
	lcd_unset_energy();
	energy_xxx_unset(wifi_probes, wifi_probes_cnt, &wifi_flag);
	energy_xxx_unset(bt_probes, bt_probes_cnt, &energy_bt_flag);

	swap_unregister_kretprobe(&switch_to_krp);
	swap_unregister_kretprobe(&sys_write_krp);
	swap_unregister_kretprobe(&sys_read_krp);

	uninit_data_energy();
}

static DEFINE_MUTEX(mutex_enable);
static int energy_enable;

/**
 * @brief Start measuring the energy consumption
 *
 * @return Error code
 */
int set_energy(void)
{
	int ret = -EINVAL;

	mutex_lock(&mutex_enable);
	if (energy_enable) {
		printk(KERN_INFO "energy profiling is already run!\n");
		goto unlock;
	}

	ret = do_set_energy();
	if (ret == 0)
		energy_enable = 1;

unlock:
	mutex_unlock(&mutex_enable);

	return ret;
}
EXPORT_SYMBOL_GPL(set_energy);

/**
 * @brief Stop measuring the energy consumption
 *
 * @return Error code
 */
int unset_energy(void)
{
	int ret = 0;

	mutex_lock(&mutex_enable);
	if (energy_enable == 0) {
		printk(KERN_INFO "energy profiling is not running!\n");
		ret = -EINVAL;
		goto unlock;
	}

	do_unset_energy();

	energy_enable = 0;
unlock:
	mutex_unlock(&mutex_enable);

	return ret;
}
EXPORT_SYMBOL_GPL(unset_energy);

int energy_once(void)
{
	const char *sym;

	sym = "__switch_to";
	switch_to_krp.kp.addr = (kprobe_opcode_t *)swap_ksyms(sym);
	if (switch_to_krp.kp.addr == NULL)
		goto not_found;

	sym = "sys_read";
	sys_read_krp.kp.addr = (kprobe_opcode_t *)swap_ksyms(sym);
	if (sys_read_krp.kp.addr == NULL)
		goto not_found;

	sym = "sys_write";
	sys_write_krp.kp.addr = (kprobe_opcode_t *)swap_ksyms(sym);
	if (sys_write_krp.kp.addr == NULL)
		goto not_found;

	energy_xxx_once(bt_probes, bt_probes_cnt);
	energy_xxx_once(wifi_probes, wifi_probes_cnt);

	return 0;

not_found:
	printk(KERN_INFO "ERROR: symbol '%s' not found\n", sym);
	return -ESRCH;
}

/**
 * @brief Initialization energy
 *
 * @return Error code
 */
int energy_init(void)
{
	int ret;

	ret = init_feature();
	if (ret)
		printk(KERN_INFO "Cannot init feature\n");

	return ret;
}

/**
 * @brief Deinitialization energy
 *
 * @return Void
 */
void energy_uninit(void)
{
	uninit_feature();

	if (energy_enable)
		do_unset_energy();
}
