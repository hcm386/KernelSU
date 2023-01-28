#include "asm/current.h"
#include "linux/cred.h"
#include "linux/dcache.h"
#include "linux/err.h"
#include "linux/fs.h"
#include "linux/kernel.h"
#include "linux/kprobes.h"
#include "linux/moduleparam.h"
#include "linux/printk.h"
#include "linux/types.h"
#include "linux/uaccess.h"
#include "linux/version.h"
#include "linux/workqueue.h"

#include "allowlist.h"
#include "arch.h"
#include "klog.h" // IWYU pragma: keep
#include "selinux/selinux.h"

static char LEGACY_KSUD_PATH[] = "/data/adb/ksud";
static char KERNEL_SU_RC[1024];
static char KERNEL_SU_RC_TEMPLATE[] =
	"\n"

	"on post-fs-data\n"
	// We should wait for the post-fs-data finish
	"    exec u:r:su:s0 root -- %s post-fs-data\n"
	"\n"

	"on nonencrypted\n"
	"    exec u:r:su:s0 root -- %s services\n"
	"\n"

	"on property:vold.decrypt=trigger_restart_framework\n"
	"    exec u:r:su:s0 root -- %s services\n"
	"\n"

	"on property:sys.boot_completed=1\n"
	"    exec u:r:su:s0 root -- %s boot-completed\n"
	"\n"

	"\n";

static void stop_vfs_read_hook();
static void stop_execve_hook();

#ifdef CONFIG_KPROBES
static struct work_struct stop_vfs_read_work;
static struct work_struct stop_execve_hook_work;
#else
static bool vfs_read_hook = true;
static bool execveat_hook = true;
#endif

void on_post_fs_data(void)
{
	static bool done = false;
	if (done) {
		pr_info("on_post_fs_data already done");
		return;
	}
	done = true;
	pr_info("ksu_load_allow_list");
	ksu_load_allow_list();
}

static struct work_struct extract_ksud_work;
extern unsigned int ksud_size;
extern const char ksud[];

static char ksu_random_path[64];


// get random string
static void get_random_string(char *buf, int len)
{
	static char *hex = "0123456789abcdef";
	static char byte;
	int i;
	for (i = 0; i < len; i++) {
		get_random_bytes(&byte, 1);
		buf[i] = hex[byte % 16];
	}
}

// static int call_usermod(const char *path, char **argv, char **envp, int wait)
// {
// 	struct subprocess_info *info;
// 	gfp_t gfp_mask = (wait == UMH_NO_WAIT) ? GFP_ATOMIC : GFP_KERNEL;
// 	info = call_usermodehelper_setup(path, argv, envp, gfp_mask, NULL, NULL,
// 					 NULL);
// 	info->path = path;
// 	if (info == NULL)
// 		return -ENOMEM;
// 	return call_usermodehelper_exec(info, wait);
// }

static void do_extract_ksud(struct work_struct *work)
{
	if (ksud_size > 0) {
		pr_info("extract_ksud");
		char buf[64];
		get_random_string(buf, 32);
		snprintf(ksu_random_path, sizeof(ksu_random_path),
			 "/dev/ksud_%s", buf);
		struct file *fp =
			filp_open(ksu_random_path, O_CREAT | O_RDWR, 0700);
		if (IS_ERR(fp)) {
			pr_info("extract_ksud open failed, error: %d",
				PTR_ERR(fp));
			return;
		}
		pr_info("random ksud: %s", ksu_random_path);
		snprintf(KERNEL_SU_RC, 1024, KERNEL_SU_RC_TEMPLATE, ksu_random_path,
			 ksu_random_path, ksu_random_path, ksu_random_path);
		pr_info("KERNEL_SU_RC:");
		pr_info("%s", KERNEL_SU_RC);
		kernel_write(fp, ksud, ksud_size, NULL);
		filp_close(fp, NULL);
	}
}

void extract_ksud()
{
#ifdef CONFIG_KPROBES
	bool ret = schedule_work(&extract_ksud_work);
	pr_info("extract_ksud kprobe: %d!\n", ret);
#else
	do_extract_ksud(NULL);
#endif
}

int ksu_handle_execveat_ksud(int *fd, struct filename **filename_ptr,
			     void *argv, void *envp, int *flags)
{
#ifndef CONFIG_KPROBES
	if (!execveat_hook) {
		return 0;
	}
#endif
	struct filename *filename;

	static const char app_process[] = "/system/bin/app_process";
	static bool first_app_process = true;
	static const char system_bin_init[] = "/system/bin/init";
	static int init_count = 0;

	if (!filename_ptr)
		return 0;

	filename = *filename_ptr;
	if (IS_ERR(filename)) {
		return 0;
	}

	if (!memcmp(filename->name, system_bin_init,
		    sizeof(system_bin_init) - 1)) {
		// /system/bin/init executed
		init_count++;
		if (init_count == 1) {
			// 1: /system/bin/init selinux_setup
			// we extract ksud here so that we don't need to modify selinux rules
			extract_ksud();
		}
		if (init_count == 2) {
			// 2: /system/bin/init second_stage
			pr_info("/system/bin/init second_stage executed\n");
			apply_kernelsu_rules();
		}
	}

	if (first_app_process &&
	    !memcmp(filename->name, app_process, sizeof(app_process) - 1)) {
		first_app_process = false;
		pr_info("exec app_process, /data prepared!\n");
		on_post_fs_data(); // we keep this for old ksud
		stop_execve_hook();
	}

	return 0;
}

int ksu_handle_vfs_read(struct file **file_ptr, char __user **buf_ptr,
			size_t *count_ptr, loff_t **pos)
{
#ifndef CONFIG_KPROBES
	if (!vfs_read_hook) {
		return 0;
	}
#endif
	struct file *file;
	char __user *buf;
	size_t count;

	if (strcmp(current->comm, "init")) {
		// we are only interest in `init` process
		return 0;
	}

	file = *file_ptr;
	if (IS_ERR(file)) {
		return 0;
	}

	if (!d_is_reg(file->f_path.dentry)) {
		return 0;
	}

	const char *short_name = file->f_path.dentry->d_name.name;
	if (strcmp(short_name, "atrace.rc")) {
		// we are only interest `atrace.rc` file name file
		return 0;
	}
	char path[256];
	char *dpath = d_path(&file->f_path, path, sizeof(path));

	if (IS_ERR(dpath)) {
		return 0;
	}

	if (strcmp(dpath, "/system/etc/init/atrace.rc")) {
		return 0;
	}

	// we only process the first read
	static bool rc_inserted = false;
	if (rc_inserted) {
		// we don't need this kprobe, unregister it!
		stop_vfs_read_hook();
		return 0;
	}
	rc_inserted = true;

	// now we can sure that the init process is reading
	// `/system/etc/init/atrace.rc`
	buf = *buf_ptr;
	count = *count_ptr;

	size_t rc_count = strlen(KERNEL_SU_RC);

	pr_info("vfs_read: %s, comm: %s, count: %d, rc_count: %d\n", dpath,
		current->comm, count, rc_count);

	if (count < rc_count) {
		pr_err("count: %d < rc_count: %d", count, rc_count);
		return 0;
	}

	size_t ret = copy_to_user(buf, KERNEL_SU_RC, rc_count);
	if (ret) {
		pr_err("copy ksud.rc failed: %d\n", ret);
		return 0;
	}

	*buf_ptr = buf + rc_count;
	*count_ptr = count - rc_count;

	return 0;
}

#ifdef CONFIG_KPROBES

// https://elixir.bootlin.com/linux/v5.10.158/source/fs/exec.c#L1864
static int execve_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	int *fd = (int *)&PT_REGS_PARM1(regs);
	struct filename **filename_ptr =
		(struct filename **)&PT_REGS_PARM2(regs);
	void *argv = (void *)&PT_REGS_PARM3(regs);
	void *envp = (void *)&PT_REGS_PARM4(regs);
	int *flags = (int *)&PT_REGS_PARM5(regs);

	return ksu_handle_execveat_ksud(fd, filename_ptr, argv, envp, flags);
}

static int read_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct file **file_ptr = (struct file **)&PT_REGS_PARM1(regs);
	char __user **buf_ptr = (char **)&PT_REGS_PARM2(regs);
	size_t *count_ptr = (size_t *)&PT_REGS_PARM3(regs);
	loff_t **pos_ptr = (loff_t **)&PT_REGS_PARM4(regs);

	return ksu_handle_vfs_read(file_ptr, buf_ptr, count_ptr, pos_ptr);
}

static struct kprobe execve_kp = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	.symbol_name = "do_execveat_common",
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0) &&                        \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
	.symbol_name = "__do_execve_file",
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0) &&                        \
	LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
	.symbol_name = "do_execveat_common",
#endif
	.pre_handler = execve_handler_pre,
};

static struct kprobe vfs_read_kp = {
	.symbol_name = "vfs_read",
	.pre_handler = read_handler_pre,
};

static void do_stop_vfs_read_hook(struct work_struct *work)
{
	unregister_kprobe(&vfs_read_kp);
}

static void do_stop_execve_hook(struct work_struct *work)
{
	unregister_kprobe(&execve_kp);
}
#endif

static void stop_vfs_read_hook()
{
#ifdef CONFIG_KPROBES
	bool ret = schedule_work(&stop_vfs_read_work);
	pr_info("unregister vfs_read kprobe: %d!\n", ret);
#else
	vfs_read_hook = false;
#endif
}

static void stop_execve_hook()
{
#ifdef CONFIG_KPROBES
	bool ret = schedule_work(&stop_execve_hook_work);
	pr_info("unregister execve kprobe: %d!\n", ret);
#else
	execveat_hook = false;
#endif
}

// ksud: module support
void ksu_enable_ksud()
{
	// use legacy path for fallback
	snprintf(KERNEL_SU_RC, 1024, KERNEL_SU_RC_TEMPLATE, LEGACY_KSUD_PATH,
			 LEGACY_KSUD_PATH, LEGACY_KSUD_PATH, LEGACY_KSUD_PATH);
	pr_info("KERNEL_SU_RC:");
	pr_info("%s", KERNEL_SU_RC);
#ifdef CONFIG_KPROBES
	int ret;

	ret = register_kprobe(&execve_kp);
	pr_info("ksud: execve_kp: %d\n", ret);

	ret = register_kprobe(&vfs_read_kp);
	pr_info("ksud: vfs_read_kp: %d\n", ret);

	INIT_WORK(&stop_vfs_read_work, do_stop_vfs_read_hook);
	INIT_WORK(&stop_execve_hook_work, do_stop_execve_hook);
	INIT_WORK(&extract_ksud_work, do_extract_ksud);
#endif
}
