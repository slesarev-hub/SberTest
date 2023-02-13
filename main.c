#include <fcntl.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

#define SHM_NAME "/shared-mem"
#define SHM_MTX "/shm_mtx"
#define MTX1 "/mtx1"
#define MTX2 "/mtx2"
#define CV1 "/cv1"
#define CV2 "/cv2"

// shm status
#define READ1 1
#define READ2 2
#define WRITE 3

#define OPEN_FLAG O_RDWR
#define CREATE_FLAG (O_RDWR | O_CREAT | O_EXCL)

#define OPEN_MODE 0
#define CREATE_MODE 0660

void sys_error(char *msg) {
  perror(msg);
  exit(1);
}

void error(char *msg) {
  printf("%s\n", msg);
  exit(1);
}

struct shm_data {
  char *buf;
  size_t buf_size;
  char *data;
  size_t data_size;
  int* status;
};

struct shm_mtx {
  pthread_mutex_t* mtx;
  pthread_mutexattr_t* attr;
  int fd;
};

struct shm_cv {
  pthread_cond_t* cv;
  pthread_condattr_t* attr;
  int fd;
};

struct interprocess_sync {
  int id;
  struct shm_mtx* mtx;
  struct shm_cv* cv;
};

struct msg_handler_args {
  struct shm_data* data;
  struct interprocess_sync* sync;
};

char* shm_map_data_pointer(size_t shm_data_size, int shm_fd) {
  char *shm_data_begin;
  if ((shm_data_begin = mmap(NULL, shm_data_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)) == MAP_FAILED) {
    sys_error("mmap");
  }
  return shm_data_begin;
}

struct shm_mtx* shm_mtx_init(char* mtx_name, int flag, mode_t mode) {
  struct shm_mtx* mtx = malloc(sizeof(struct shm_mtx));
  mtx->attr = malloc(sizeof(pthread_mutexattr_t));
  
  if ((mtx->fd = shm_open(mtx_name, flag, mode)) == -1) {
    sys_error("shm_mtx_init, shm_open, mtx");
  }
  if (flag == CREATE_FLAG) {
    if (ftruncate(mtx->fd, sizeof(pthread_mutex_t)) == -1) {
      sys_error("ftruncate");
    }
  }
  mtx->mtx = (pthread_mutex_t*) shm_map_data_pointer(sizeof(pthread_mutex_t), mtx->fd);
  
  pthread_mutexattr_setpshared(mtx->attr, PTHREAD_PROCESS_SHARED);
  pthread_mutex_init(mtx->mtx, mtx->attr);

  return mtx;
}

struct shm_cv* shm_cv_init(char* cv_name, int flag, mode_t mode) {
  struct shm_cv* cv = malloc(sizeof(struct shm_cv));
  cv->attr = malloc(sizeof(pthread_condattr_t));
  
  if ((cv->fd = shm_open(cv_name, flag, mode)) == -1) {
    sys_error("shm_cv_init, shm_open, cv");
  }
  if (flag == CREATE_FLAG) {
    if (ftruncate(cv->fd, sizeof(pthread_cond_t)) == -1) {
      sys_error("ftruncate");
    }
  }
  cv->cv = (pthread_cond_t*) shm_map_data_pointer(sizeof(pthread_cond_t), cv->fd);
  
  pthread_condattr_setpshared(cv->attr, PTHREAD_PROCESS_SHARED);
  pthread_cond_init(cv->cv, cv->attr);

  return cv;
}

struct interprocess_sync* shm_init_sync(char* cv_name, char* mtx_name, int id, int flag, mode_t mode) {
  struct interprocess_sync* sync = malloc(sizeof(struct interprocess_sync));
  sync->id = id;

  sync->cv = shm_cv_init(cv_name, flag, mode);
  sync->mtx = shm_mtx_init(mtx_name, flag, mode);

  return sync;
}

char *write_line_to_shm(struct msg_handler_args* args) {
  char *new_buf = args->data->data;
  char c = getchar();
  pthread_mutex_lock(args->sync->mtx->mtx);
  do {
    if (new_buf + 1 == args->data->data + args->data->data_size) {
      error("buf");
    }
    *new_buf = c;
    new_buf++;
  } while ((c = getchar()) != '\n');
  *new_buf = '\0';
  new_buf++;
  *args->data->status = args->sync->id;
  pthread_cond_signal(args->sync->cv->cv);
  pthread_mutex_unlock(args->sync->mtx->mtx);
  return new_buf;
}

int read_line_from_other_proc(struct msg_handler_args* args) {
  pthread_mutex_lock(args->sync->mtx->mtx);

  while(*args->data->status != args->sync->id) {
    pthread_cond_wait(args->sync->cv->cv, args->sync->mtx->mtx);
  }
  printf("%s\n", args->data->data);
  *args->data->status = WRITE;
  pthread_mutex_unlock(args->sync->mtx->mtx);
  return 1;
}

void process_read(void* args) {
  printf("%d %d\n",syscall(__NR_gettid),getpid());
  while (read_line_from_other_proc((struct msg_handler_args*)args)) {}
}

void process_write(void* args) {
  printf("%d %d\n",syscall(__NR_gettid),getpid());
  while (write_line_to_shm((struct msg_handler_args*)args)) {}
}

void process_messages(struct shm_data* data, struct interprocess_sync* reader_sync, struct interprocess_sync* writer_sync) {
  struct msg_handler_args* write_args = malloc(sizeof(struct msg_handler_args));
  write_args->data = data;
  write_args->sync = writer_sync;  
  pthread_t write_thread;
  pthread_create(&write_thread, NULL, process_write, (void*)write_args);
  
  struct msg_handler_args* read_args = malloc(sizeof(struct msg_handler_args));
  read_args->data = data;
  read_args->sync = reader_sync;
  process_read((void*)read_args);
}

struct shm_data* init_data(char* shm_data_begin, size_t shm_data_size, int flag) {
  struct shm_data* data = malloc(sizeof(struct shm_data));
  data->buf = shm_data_begin;
  data->buf_size = shm_data_size;
  data->data = shm_data_begin + 4;
  data->data_size = shm_data_size - 4;
  data->status = (int*)shm_data_begin;
  if (flag == CREATE_FLAG) {
    *data->status = WRITE;
  }
  return data;
}

//pthread_cond_destroy(cv.pcond);
//pthread_condattr_destroy(&cv.attrcond); 

int main(int argc, char **argv) {
  char *shm_data_begin;
  char *shm_data_free;
  size_t shm_data_size = 10;
  int shm_fd;
  sem_t *shm_mtx;

  printf("%d\n", getppid());
  printf("%d\n", getpid());

  if ((shm_mtx = sem_open(SHM_MTX, 0, 0, 0)) == SEM_FAILED) {
    if ((shm_mtx = sem_open(SHM_MTX, O_CREAT, 0660, 0)) == SEM_FAILED) {
      sys_error("sem_open, cannot create");
    }
  }

  if ((shm_fd = shm_open(SHM_NAME, OPEN_FLAG, OPEN_MODE)) == -1) {
    if ((shm_fd = shm_open(SHM_NAME, CREATE_FLAG, CREATE_MODE)) == -1) {
      sys_error("shm_open, cannot create");
    }
    if (ftruncate(shm_fd, shm_data_size) == -1) {
      sys_error("ftruncate");
    }
    if (sem_post(shm_mtx) == -1) {
      sys_error("sem_post: mutex_sem");
    }
    struct interprocess_sync* sync1 = shm_init_sync(CV1, MTX1, READ1, CREATE_FLAG, CREATE_MODE);
    struct interprocess_sync* sync2 = shm_init_sync(CV2, MTX2, READ2, CREATE_FLAG, CREATE_MODE);
    shm_data_begin = shm_map_data_pointer(shm_data_size, shm_fd);
    process_messages(init_data(shm_data_begin, shm_data_size, CREATE_FLAG), sync1, sync2);
  }

  struct interprocess_sync* sync1 = shm_init_sync(CV1, MTX1, READ1, OPEN_FLAG, OPEN_MODE);
  struct interprocess_sync* sync2 = shm_init_sync(CV2, MTX2, READ2, OPEN_FLAG, OPEN_MODE);
  shm_data_begin = shm_map_data_pointer(shm_data_size, shm_fd);
  process_messages(init_data(shm_data_begin, shm_data_size, OPEN_FLAG), sync2, sync1);
}