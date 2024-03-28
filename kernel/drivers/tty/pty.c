#include <drivers/tty/tty.h>
#include <drivers/tty/pty.h>
#include <fs/cdev.h>
#include <fs/fd.h>
#include <cpu.h>
#include <types.h>
#include <termios.h>
#include <ioctl.h>
#include <errno.h>
#include <circular_queue.h>
#include <debug.h>
#include <lock.h>

#define PTMX_MAJOR 5
#define PTMX_MINOR 2

#define PTS_MAJOR 136

// Master/slave metadata fields
struct ptm_data;
struct pts_data {
	int slave_no;
	struct tty *tty;
	struct ptm_data *master;
	struct winsize winsize;
};

struct ptm_data {
	struct spinlock input_lock;
	struct circular_queue input_queue;
	struct pts_data *slave;
};

// Global state
static struct bitmap pts_bitmap = {
	.resizable = true
};

static struct spinlock pty_lock;

// Driver operations
static int ptmx_open(struct vfs_node *node, struct file_handle *file, int);
static ssize_t ptm_read(struct file_handle *file, void *buf, size_t count, off_t);
static ssize_t ptm_write(struct file_handle *file, const void *buf, size_t count, off_t);
static int ptm_ioctl(struct file_handle *file, uint64_t req, void *arg);

static void pts_flush_output(struct tty *tty);
static int pts_ioctl(struct tty *tty, uint64_t req, void *arg);

static struct file_ops ptmx_ops = {
	.open = ptmx_open
};

static struct file_ops ptm_ops = {
	.read = ptm_read,
	.write = ptm_write,
	.ioctl = ptm_ioctl
};

static struct tty_ops pts_ops = {
	.flush_output = pts_flush_output,
	.ioctl = pts_ioctl
};

static struct tty_driver pts_driver = {
	.ops = &pts_ops
};

// Implementation

int pty_init() {
	struct cdev *cdev = alloc(sizeof(struct cdev));
	cdev->fops = &ptmx_ops;
	cdev->rdev = makedev(PTMX_MAJOR, PTMX_MINOR);
	if(cdev_register(cdev) == -1) {
		return -1;
	}

	struct stat *stat = alloc(sizeof(struct stat));
	stat_init(stat);
	stat->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
	stat->st_rdev = makedev(PTMX_MAJOR, PTMX_MINOR);
	vfs_create_node_deep(NULL, NULL, NULL, stat, "/dev/ptmx");
	return 0;
}

static int ptmx_open(struct vfs_node*, struct file_handle *file, int) {
	spinlock_irqsave(&pty_lock);

	int slave_no = bitmap_alloc(&pts_bitmap);

	struct tty *pts_tty = alloc(sizeof(struct tty));
	struct pts_data *pts_data = alloc(sizeof(struct pts_data));
	struct ptm_data *ptm_data = alloc(sizeof(struct ptm_data));
	struct stat *pts_stat = alloc(sizeof(struct stat));
	struct stat *ptm_stat = alloc(sizeof(struct stat));

	pts_tty->driver = &pts_driver;
	pts_tty->private_data = pts_data;
	pts_data->slave_no = slave_no;
	pts_data->tty = pts_tty;
	pts_data->master = ptm_data;

	circular_queue_init(&ptm_data->input_queue, MAX_LINE, sizeof(char));
	ptm_data->slave = pts_data;

	stat_init(pts_stat);
	pts_stat->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IWGRP;
	pts_stat->st_rdev = makedev(PTS_MAJOR, slave_no);
	pts_stat->st_uid = CURRENT_TASK->effective_uid;
	pts_stat->st_gid = CURRENT_TASK->effective_gid;

	stat_init(ptm_stat);
	ptm_stat->st_mode = S_IRUSR | S_IWUSR;
	ptm_stat->st_uid = CURRENT_TASK->effective_uid;
	ptm_stat->st_gid = CURRENT_TASK->effective_gid;
	file->stat = ptm_stat;
	file->ops = &ptm_ops;
	file->private_data = ptm_data;
	file->vfs_node = NULL;

	tty_register(makedev(PTS_MAJOR, slave_no), pts_tty);
	char *pts_name = alloc(MAX_PATH_LENGTH);
	sprint(pts_name, "/dev/pts/%d", slave_no);
	vfs_create_node_deep(NULL, NULL, NULL, pts_stat, pts_name);

	spinrelease_irqsave(&pty_lock);
	return 0;
}

static void pts_flush_output(struct tty *tty) {
	struct pts_data *pts = tty->private_data;
	struct ptm_data *ptm = pts->master;
	char ch;

	spinlock_irqsave(&tty->output_lock);
	spinlock_irqsave(&ptm->input_lock);

	while(circular_queue_pop(&tty->output_queue, &ch)) {
		if(!circular_queue_push(&ptm->input_queue, &ch)) {
			break;
		}
	}

	spinrelease_irqsave(&ptm->input_lock);
	spinrelease_irqsave(&tty->output_lock);
}

static ssize_t ptm_read(struct file_handle *file, void *buf, size_t count, off_t) {
	struct ptm_data *ptm = file->private_data;
	ssize_t ret;
	char *c_buf = buf;

	spinlock_irqsave(&ptm->input_lock);

	for(ret = 0; ret < (ssize_t)count; ret++) {
		if(!circular_queue_pop(&ptm->input_queue, c_buf)) {
			break;
		}
		c_buf++;
	}

	spinrelease_irqsave(&ptm->input_lock);
	return ret;
}

static ssize_t ptm_write(struct file_handle *file, const void *buf, size_t count, off_t) {
	struct ptm_data *ptm = file->private_data;
	struct pts_data *pts = ptm->slave;
	ssize_t ret;
	const char *c_buf = buf;

	spinlock_irqsave(&pts->tty->input_lock);

	for(ret = 0; ret < (ssize_t)count; ret++) {
		if(!circular_queue_push(&pts->tty->input_queue, c_buf)) {
			break;
		}
		c_buf++;
	}

	spinrelease_irqsave(&pts->tty->input_lock);
	return ret;
}

static int ptm_ioctl(struct file_handle *file, uint64_t req, void *arg) {
	struct ptm_data *ptm = file->private_data;
	struct pts_data *pts = ptm->slave;

	switch(req) {
		case TIOCGPTN:
#if defined(SYSCALL_DEBUG_FD)
			print("syscall: [pid %x, tid %x] pty_ioctl: TIOCGPTN\n", CORE_LOCAL->pid, CORE_LOCAL->tid);
#endif
			int *ptn = arg;
			*ptn = pts->slave_no;
			return 0;

		case TIOCGWINSZ: {
#if defined(SYSCALL_DEBUG_FD)
			print("syscall: [pid %x, tid %x] pty_ioctl: TIOCGWINSZ\n", CORE_LOCAL->pid, CORE_LOCAL->tid);
#endif
			memcpy(arg, &pts->winsize, sizeof(struct winsize));
			return 0;
		}

		case TIOCSWINSZ: {
#if defined(SYSCALL_DEBUG_FD)
			print("syscall: [pid %x, tid %x] pty_ioctl: TIOCGWINSZ\n", CORE_LOCAL->pid, CORE_LOCAL->tid);
#endif
			memcpy(&pts->winsize, arg, sizeof(struct winsize));
			// TODO: inject signal.
			return 0;
		}

		/*default:
			set_errno(ENOSYS);
			return -1;*/
	}

	return 0;
}

static int pts_ioctl(struct tty *tty, uint64_t req, void *arg) {
	struct pts_data *pts = tty->private_data;

	switch(req) {
		case TIOCGWINSZ: {
#if defined(SYSCALL_DEBUG_FD)
			print("syscall: [pid %x, tid %x] pty_ioctl: TIOCGWINSZ\n", CORE_LOCAL->pid, CORE_LOCAL->tid);
#endif
			memcpy(arg, &pts->winsize, sizeof(struct winsize));
			return 0;
		}

		case TIOCSWINSZ: {
#if defined(SYSCALL_DEBUG_FD)
			print("syscall: [pid %x, tid %x] pty_ioctl: TIOCGWINSZ\n", CORE_LOCAL->pid, CORE_LOCAL->tid);
#endif
			memcpy(&pts->winsize, arg, sizeof(struct winsize));
			// TODO: inject signal.
			return 0;
		}

		default:
			set_errno(ENOSYS);
			return -1;
	}
}
