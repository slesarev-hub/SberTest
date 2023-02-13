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
#define SHM_MUTEX "/sem-mutex"
#define MTX1 "/mtx1"
#define MTX2 "/mtx2"
#define CV1 "/cv1"
#define CV2 "/cv2"

// shm status
#define READ1 1
#define READ2 2
#define WRITE 3

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

struct interprocess_sync {
  int id;

  pthread_mutex_t* mtx;
  pthread_mutexattr_t* mtx_attr;
  int mtx_fd;

  pthread_cond_t* cond;
  pthread_condattr_t* cond_attr;
  int cond_fd;
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

struct interprocess_sync* shm_open_sync(char* cv_name, char* mtx_name, int id) {
  struct interprocess_sync* sync = malloc(sizeof(struct interprocess_sync));
  sync->id = id;

  if ((sync->mtx_fd = shm_open(mtx_name, O_RDWR, 0)) == -1) {
    sys_error("shm_open_sync, shm_open, mtx");
  }
  sync->mtx = (pthread_mutex_t*) shm_map_data_pointer(sizeof(pthread_mutex_t), sync->mtx_fd);
  
  if ((sync->cond_fd = shm_open(cv_name, O_RDWR, 0)) == -1) {
    sys_error("shm_open_sync, shm_open, cv");
  }
  sync->cond = (pthread_cond_t*) shm_map_data_pointer(sizeof(pthread_cond_t), sync->cond_fd);
  return sync;
}

struct interprocess_sync* shm_init_sync(char* cv_name, char* mtx_name, int id) {
  struct interprocess_sync* sync = malloc(sizeof(struct interprocess_sync));
  sync->id = id;

  if ((sync->mtx_fd = shm_open(mtx_name, O_RDWR | O_CREAT | O_EXCL, 0660)) == -1) {
    sys_error("shm_init_sync, shm_open, mtx");
  }
  if (ftruncate(sync->mtx_fd, sizeof(pthread_mutex_t)) == -1) {
      sys_error("ftruncate");
  }
  sync->mtx = (pthread_mutex_t*) shm_map_data_pointer(sizeof(pthread_mutex_t), sync->mtx_fd);
  
  sync->mtx_attr = malloc(sizeof(pthread_mutexattr_t));
  pthread_mutexattr_setpshared(sync->mtx_attr, PTHREAD_PROCESS_SHARED);
  pthread_mutex_init(sync->mtx, sync->mtx_attr);
  
  if ((sync->cond_fd = shm_open(cv_name, O_RDWR | O_CREAT | O_EXCL, 0660)) == -1) {
    sys_error("shm_init_sync, shm_open, cv");
  }
  if (ftruncate(sync->cond_fd, sizeof(pthread_cond_t)) == -1) {
      sys_error("ftruncate");
  }
  sync->cond = (pthread_cond_t*) shm_map_data_pointer(sizeof(pthread_cond_t), sync->cond_fd);
  
  sync->cond_attr = malloc(sizeof(pthread_condattr_t));
  pthread_condattr_setpshared(sync->cond_attr, PTHREAD_PROCESS_SHARED);
  pthread_cond_init(sync->cond, sync->cond_attr);
  return sync;
};

char *write_line_to_shm(struct msg_handler_args* args) {
  char *new_buf = args->data->data;
  char c = getchar();
  pthread_mutex_lock(args->sync->mtx);
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
  pthread_cond_signal(args->sync->cond);
  pthread_mutex_unlock(args->sync->mtx);
  return new_buf;
}

int read_line_from_other_proc(struct msg_handler_args* args) {
  pthread_mutex_lock(args->sync->mtx);

  while(*args->data->status != args->sync->id) {
    pthread_cond_wait(args->sync->cond, args->sync->mtx);
  }
  printf("%s\n", args->data->data);
  *args->data->status = WRITE;
  pthread_mutex_unlock(args->sync->mtx);
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

void process_messages(struct shm_data* data, struct interprocess_sync* reader_sync, struct interprocess_sync* writer_sync, int f) {
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

struct shm_data* init_data(char* shm_data_begin, size_t shm_data_size) {
  struct shm_data* data = malloc(sizeof(struct shm_data));
  data->buf = shm_data_begin;
  data->buf_size = shm_data_size;
  data->data = shm_data_begin + 4;
  data->data_size = shm_data_size - 4;
  data->status = (int*)shm_data_begin;
  *data->status = WRITE;
  return data;
}

//pthread_cond_destroy(cv.pcond);
//pthread_condattr_destroy(&cv.attrcond); 

int main(int argc, char **argv) {
  char *shm_data_begin;
  char *shm_data_free;
  size_t shm_data_size = 10;
  int shm_fd;
  sem_t *shm_mutex;

  printf("%d\n", getppid());
  printf("%d\n", getpid());

  if ((shm_mutex = sem_open(SHM_MUTEX, 0, 0, 0)) == SEM_FAILED) {
    if ((shm_mutex = sem_open(SHM_MUTEX, O_CREAT, 0660, 0)) == SEM_FAILED) {
      sys_error("sem_open, cannot create");
    }
  }

  if ((shm_fd = shm_open(SHM_NAME, O_RDWR, 0)) == -1) {
    if ((shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0660)) == -1) {
      sys_error("shm_open, cannot create");
    }
    if (ftruncate(shm_fd, shm_data_size) == -1) {
      sys_error("ftruncate");
    }
    if (sem_post(shm_mutex) == -1) {
      sys_error("sem_post: mutex_sem");
    }
    struct interprocess_sync* sync1 = shm_init_sync(CV1, MTX1, READ1);
    struct interprocess_sync* sync2 = shm_init_sync(CV2, MTX2, READ2);
    shm_data_begin = shm_map_data_pointer(shm_data_size, shm_fd);
    process_messages(init_data(shm_data_begin, shm_data_size), sync1, sync2, 1);
  }

  struct interprocess_sync* sync1 = shm_open_sync(CV1, MTX1, READ1);
  struct interprocess_sync* sync2 = shm_open_sync(CV2, MTX2, READ2);
  shm_data_begin = shm_map_data_pointer(shm_data_size, shm_fd);
  process_messages(init_data(shm_data_begin, shm_data_size), sync2, sync1, 2);
}