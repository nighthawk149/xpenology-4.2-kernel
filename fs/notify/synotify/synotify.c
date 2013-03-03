#include <linux/synotify.h>
#include <linux/fdtable.h>
#include <linux/fsnotify_backend.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h> /* UINT_MAX */
#include <linux/mount.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/wait.h>

static bool should_merge(struct fsnotify_event *old, struct fsnotify_event *new)
{
	pr_debug("%s: old=%p, mask=%x, new=%p, mask=%x\n", __func__, old, old->mask, new, new->mask);
	// old event is Event overflow and new event is event overflow too.
	if(old->data_type == FSNOTIFY_EVENT_NONE && new->data_type == FSNOTIFY_EVENT_NONE){
		if((old->mask & FS_Q_OVERFLOW) && (new->mask & FS_Q_OVERFLOW))
				return true;
		return false;
	}else
		return false;

	if (old->data_type == new->data_type &&
	    (old->full_name_len == new->full_name_len) &&
	    (old->path.mnt == new->path.mnt)) {
		switch (old->data_type) {
		case (FSNOTIFY_EVENT_SYNO):
			/* MOVE_FROM & MOVE_TO will assign dentry as NULL, and those two event should not be merged */
			if (strcmp(old->full_name, new->full_name)==0)
			    return true;
			break;
		case (FSNOTIFY_EVENT_PATH):
			if (old->path.dentry == new->path.dentry)
				return true;
			break;
		case (FSNOTIFY_EVENT_NONE):
			return true;
		default:
			BUG();
		};
	}
	return false;
}


/* and the list better be locked by something too! */
static struct fsnotify_event *synotify_merge(struct list_head *list,
					     struct fsnotify_event *event)
{
	struct fsnotify_event_holder *test_holder;
	struct fsnotify_event *test_event = NULL;
	struct fsnotify_event *new_event;

	pr_debug("%s: list=%p event=%p, mask=%x\n", __func__, list, event, event->mask);


	list_for_each_entry_reverse(test_holder, list, event_list) {
		if (should_merge(test_holder->event, event)) {
			test_event = test_holder->event;
			break;
		}
	}

	if (!test_event){
		return NULL;
	}

	fsnotify_get_event(test_event);

	/* if they are exactly the same we are done */
	if (test_event->mask == event->mask)
		return test_event;

	/*
	 * if the refcnt == 2 this is the only queue
	 * for this event and so we can update the mask
	 * in place.
	 */
	if (atomic_read(&test_event->refcnt) == 2) {
		test_event->mask |= event->mask;
		return test_event;
	}

	new_event = fsnotify_clone_event(test_event);

	/* done with test_event */
	fsnotify_put_event(test_event);

	/* couldn't allocate memory, merge was not possible */
	if (unlikely(!new_event))
		return ERR_PTR(-ENOMEM);

	/* build new event and replace it on the list */
	new_event->mask = (test_event->mask | event->mask);
	fsnotify_replace_event(test_holder, new_event);

	/* we hold a reference on new_event from clone_event */
	return new_event;
}

static int synotify_handle_event(struct fsnotify_group *group,
				 struct fsnotify_mark *inode_mark,
				 struct fsnotify_mark *synotify_mark,
				 struct fsnotify_event *event)
{
	int ret = 0;
	struct fsnotify_event *notify_event = NULL;

	pr_debug("%s: group=%p event=%p, mnt=%p, mask=%x\n", __func__, group, event, event->path.mnt, event->mask);
	// to prevent event dependecy, we should have much clever way to do merge
	notify_event = fsnotify_add_notify_event(group, event, NULL, synotify_merge);
	if (IS_ERR(notify_event))
		return PTR_ERR(notify_event);

	if (notify_event)
		fsnotify_put_event(notify_event);

	return ret;
}

static bool synotify_should_send_event(struct fsnotify_group *group,
				       struct inode *to_tell,
				       struct fsnotify_mark *inode_mark,
				       struct fsnotify_mark *vfsmnt_mark,
				       __u32 event_mask, void *data, int data_type)
{
	__u32 marks_mask;

	pr_debug("%s: group=%p to_tell=%p inode_mark=%p vfsmnt_mark=%p "
		 "mask=%x data=%p data_type=%d\n", __func__, group, to_tell,
		 inode_mark, vfsmnt_mark, event_mask, data, data_type);

	/* if we don't have enough info to send an event to userspace say no */
	if (data_type != FSNOTIFY_EVENT_SYNO && data_type != FSNOTIFY_EVENT_PATH)
		return false;

	if (vfsmnt_mark) {
		marks_mask = vfsmnt_mark->mask;
	} else {
		BUG();
	}

	if (event_mask & marks_mask)
		return true;

	return false;
}

static void synotify_free_group_priv(struct fsnotify_group *group)
{
	struct user_struct *user;

	user = group->synotify_data.user;
	atomic_dec(&user->synotify_instances);
	free_uid(user);
}

const struct fsnotify_ops synotify_fsnotify_ops = {
	.handle_event = synotify_handle_event,
	.should_send_event = synotify_should_send_event,
	.free_group_priv = synotify_free_group_priv,
	.free_event_priv = NULL,
	.freeing_mark = NULL,
};
