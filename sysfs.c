// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2025 Philipp Jungkamp <philipp.jungkamp@rwth-aachen.de>
 * Copyright (C) 2025 Jonas Dohmen <jonas.dohmen@rwth-aachen.de>
 * Copyright (C) 2025 Antonios Salios <antonios.salios@rwth-aachen.de>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/blk_types.h>
#include <linux/fs.h>
#include <linux/kobject.h>

#include "ouichefs.h"

struct oui_object {
	struct kobject kobj;
	struct super_block *sb;
};
#define TO_OUI_OBJECT(x) container_of(x, struct oui_object, kobj)
#define OUI_OBJ_NAME(x) kobject_name(&x->kobj)

static void oui_object_release(struct kobject *kobj)
{
	struct oui_object *oui_obj = TO_OUI_OBJECT(kobj);

	kfree(oui_obj);
}

struct oui_attribute {
	struct attribute attr;
	ssize_t (*show)(struct oui_object *obj, struct oui_attribute *attr,
			char *buf);
	ssize_t (*store)(struct oui_object *obj, struct oui_attribute *attr,
			 const char *buf, size_t count);
};
#define TO_OUI_ATTRIBUTE(x) container_of(x, struct oui_attribute, attr)

static ssize_t oui_snapshot_fn(const char *buf, struct super_block *sb,
			       int (*fs_func)(struct super_block *sb,
					      uint32_t id))
{
	uint32_t id;
	int res = kstrtou32(buf, 10, &id);

	if (res)
		return res;

	return fs_func(sb, id);
}

static ssize_t create_store(struct oui_object *obj, struct oui_attribute *attr,
			    const char *buf, size_t count)
{
	ssize_t res = oui_snapshot_fn(buf, obj->sb, &ouichefs_snapshot_create);

	if (res) {
		pr_err("'%s' snapshot create: failed!\n", OUI_OBJ_NAME(obj));
		return res;
	}
	pr_info("'%s' snapshot create: success!\n", OUI_OBJ_NAME(obj));
	return count;
}
static struct oui_attribute oui_create_attr = __ATTR_WO(create);

static ssize_t destroy_store(struct oui_object *obj, struct oui_attribute *attr,
			     const char *buf, size_t count)
{
	ssize_t res = oui_snapshot_fn(buf, obj->sb, &ouichefs_snapshot_destroy);

	if (res) {
		pr_err("'%s' snapshot destroy: failed!\n", OUI_OBJ_NAME(obj));
		return res;
	}
	pr_info("'%s' snapshot destroy: success!\n", OUI_OBJ_NAME(obj));
	return count;
}
static struct oui_attribute oui_destroy_attr = __ATTR_WO(destroy);

static ssize_t restore_store(struct oui_object *obj, struct oui_attribute *attr,
			     const char *buf, size_t count)
{
	ssize_t res = oui_snapshot_fn(buf, obj->sb, &ouichefs_snapshot_restore);

	if (res) {
		pr_err("'%s' snapshot restore: failed!\n", OUI_OBJ_NAME(obj));
		return res;
	}
	pr_info("'%s' snapshot restore: success!\n", OUI_OBJ_NAME(obj));
	return count;
}
static struct oui_attribute oui_restore_attr = __ATTR_WO(restore);

#define OUTPUT_FORMAT \
	"%d: %02d.%02d.%02d %02d:%02d:%02d\n" // "ID: dd.mm.yy HH:MM:SS"
static ssize_t list_show(struct oui_object *obj, struct oui_attribute *attr,
			 char *buf)
{
	int at = 0;
	struct tm tm;

	const struct ouichefs_sb_info *sbi = OUICHEFS_SB(obj->sb);

	for (int i = 0; i < sbi->nr_snapshots; i++) {
		const struct ouichefs_snapshot *snap = &sbi->snapshots[i];

		time64_to_tm(snap->s_time, 0, &tm);
		int sz = sysfs_emit_at(buf, at, OUTPUT_FORMAT, snap->s_id,
				       tm.tm_mday, tm.tm_mon + 1,
				       (int)(tm.tm_year % 100), tm.tm_hour,
				       tm.tm_min, tm.tm_sec);
		if (!sz)
			break;
		at += sz;
	}

	return at;
}
static struct oui_attribute oui_list_attr = __ATTR_RO(list);

static struct attribute *oui_attrs[] = { &oui_create_attr.attr,
					 &oui_list_attr.attr,
					 &oui_destroy_attr.attr,
					 &oui_restore_attr.attr, NULL };
ATTRIBUTE_GROUPS(oui);

ssize_t oui_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct oui_object *oui_obj = TO_OUI_OBJECT(kobj);
	struct oui_attribute *oui_attr = TO_OUI_ATTRIBUTE(attr);

	return oui_attr->show(oui_obj, oui_attr, buf);
}

ssize_t oui_attr_store(struct kobject *kobj, struct attribute *attr,
		       const char *buf, size_t count)
{
	struct oui_object *oui_obj = TO_OUI_OBJECT(kobj);
	struct oui_attribute *oui_attr = TO_OUI_ATTRIBUTE(attr);

	return oui_attr->store(oui_obj, oui_attr, buf, count);
}

static const struct sysfs_ops oui_sysfs_ops = { .show = oui_attr_show,
						.store = oui_attr_store };

static const struct kobj_type oui_obj_type = { .sysfs_ops = &oui_sysfs_ops,
					       .default_groups = oui_groups,
					       .release = &oui_object_release };

static struct kset *oui_kset;

/*
 * Register new device to sysfs interface (i.e. on mount)
 */
int sysfs_register_dev(struct super_block *sb)
{
	int ret;
	struct oui_object *new = kzalloc(sizeof(struct oui_object), GFP_KERNEL);

	if (!new) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	const char *name = dev_name(&sb->s_bdev->bd_device);

	new->kobj.kset = oui_kset;
	new->sb = sb;
	ret = kobject_init_and_add(&new->kobj, &oui_obj_type, NULL, "%s", name);
	if (ret) {
		pr_err("Could not create kobject!\n");
		goto err_kobj_init_add;
	}

	kobject_uevent(&new->kobj, KOBJ_ADD);

	return 0;

err_kobj_init_add:
	kobject_put(&new->kobj);
err_alloc:
	return ret;
}

/*
 * Unregister device from sysfs interface (i.e. on unmount)
 */
void sysfs_unregister_dev(const struct super_block *sb)
{
	const char *name = dev_name(&sb->s_bdev->bd_device);
	struct kobject *kobj = kset_find_obj(oui_kset, name);

	if (!kobj)
		return;
	kobject_put(kobj);
	kobject_put(kobj);
}

/*
 * Initialize sysfs interface
 */
int sysfs_init(void)
{
	oui_kset = kset_create_and_add("ouichefs", NULL, fs_kobj);
	if (!oui_kset)
		return -ENOMEM;

	return 0;
}

/*
 * Uninitialize sysfs interface
 */
void sysfs_deinit(void)
{
	kset_unregister(oui_kset);
}
