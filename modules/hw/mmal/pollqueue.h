#ifndef POLLQUEUE_H_
#define POLLQUEUE_H_

#include <poll.h>

struct polltask;
struct pollqueue;

// fd       fd to poll
// events   Events to wait for (POLLxxx)
// revents  Event that triggered the callback
//          0 => timeout
// v        User pointer to callback
struct polltask *polltask_new(const int fd, const short events,
			      void (*const fn)(void *v, short revents),
			      void *const v);
// deletes the task - no locking so do not delete until not in use
void polltask_delete(struct polltask **const ppt);

// timeout_ms == -1 => never
void pollqueue_add_task(struct pollqueue *const pq, struct polltask *const pt,
			const int timeout_ms);
// Create a new pollqueue (starts thread)
struct pollqueue * pollqueue_new(void);
// Stop and delete the pollqueue
// Pending tasks may not complete but any callback that is running will complete
// Poll task will be dead by the time this returns so safe to delete all
// polltask objects after calling this
void pollqueue_delete(struct pollqueue **const ppq);

#endif /* POLLQUEUE_H_ */
