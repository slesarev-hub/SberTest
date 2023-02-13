#include <fcntl.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

// TODO Add history option
// TODO Add tests
// TODO Add shm data structure with data and garbage linked lists
// TODO Destroy allocated resources
// TODO Add memcheck test
// TODO Catch signals
// TODO Split code into modules
// TODO Try valgrind
// TODO Add defrag

#define SHM_DATA "/shared-data"

#define SHM_META "/shared-info"
#define SHM_META_SIZE (sizeof(int) + sizeof(size_t) + sizeof(int))

#define SHM_MTX "/shm_mtx"
#define RW_MTX1 "/rw_mtx1"
#define RW_MTX2 "/rw_mtx2"
#define RW_CV1 "/rw_cv1"
#define RW_CV2 "/rw_cv2"
#define W_MTX "/w_mtx"

// shm status, stored in SHM_META
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

struct options {
  size_t shm_data_size;
};

struct shm_storage {
  char* free_space;
  char* begin_space;
  char* end_space;

  size_t free_space_size;

  int* status;
  char* first_record;
  int* record_count;
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
  struct shm_storage* storage;
  struct interprocess_sync* sync;
  struct shm_mtx* write_mtx;
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
  if (flag == CREATE_FLAG) {
    pthread_mutexattr_setpshared(mtx->attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(mtx->mtx, mtx->attr);
  }
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
  
  if (flag == CREATE_FLAG) {
    pthread_condattr_setpshared(cv->attr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(cv->cv, cv->attr);
  }
  return cv;
}

struct interprocess_sync* shm_init_sync(char* cv_name, char* mtx_name, int id, int flag, mode_t mode) {
  struct interprocess_sync* sync = malloc(sizeof(struct interprocess_sync));
  sync->id = id;

  sync->cv = shm_cv_init(cv_name, flag, mode);
  sync->mtx = shm_mtx_init(mtx_name, flag, mode);

  return sync;
}

void move_next(struct shm_storage* storage) {
  char* record = storage->first_record;
  while(*record != '\0') {
    while (*record != '\n') {
      record++;
      storage->free_space_size++;
    }
    record = *(size_t*)record;
  }
  record++;
  storage->first_record = *(size_t*)record;
  (*storage->record_count)--;
}

void write_line_to_shm(struct msg_handler_args* args) {
  while(*args->storage->status != WRITE) {}
  char c = getchar();
  pthread_mutex_lock(args->write_mtx->mtx);
  pthread_mutex_lock(args->sync->mtx->mtx);
  struct shm_storage* storage = args->storage;
  char* new_record_begin = storage->free_space;
  char* new_record = storage->free_space;
  while((c != '\n') && (c != EOF)) {
    if (storage->free_space_size > 0) {
      while (*new_record == '\n') {
        new_record++;
        new_record = *(size_t*)new_record;
      }
      *new_record = c;
      new_record++;
      storage->free_space_size--;
    } else if (storage->record_count > 0) {
      *new_record = '\n';
      new_record++;
      *(size_t*)new_record = storage->first_record;
      new_record = storage->first_record;
      move_next(storage);
    } else {
      error("too long input");
    }
    c = getchar();
  }

  *new_record = '\0';
  new_record++;
  if (storage->free_space_size > 0)//TODO проверить, что есть место на два перехода

  if (storage->first_record == NULL) {
    storage->first_record = new_record_begin;
  }
  (*storage->record_count)++;
  storage->free_space = new_record;

  storage->status = args->sync->id;
  pthread_cond_signal(args->sync->cv->cv);
  pthread_mutex_unlock(args->sync->mtx->mtx);
  pthread_mutex_unlock(args->write_mtx->mtx);
}

int read_line_from_other_proc(struct msg_handler_args* args) {
  pthread_mutex_lock(args->sync->mtx->mtx);

  while(*args->storage->status != args->sync->id) {
    pthread_cond_wait(args->sync->cv->cv, args->sync->mtx->mtx);
  }
  printf("%s\n", args->storage->data);
  *args->storage->status = WRITE;
  pthread_mutex_unlock(args->sync->mtx->mtx);
  return 1;
}

void process_read(void* args) {
  // printf("%d %d\n",syscall(__NR_gettid),getpid());
  while (read_line_from_other_proc((struct msg_handler_args*)args)) {}
}

void* process_write(void* args) {
  // printf("%d %d\n",syscall(__NR_gettid),getpid());
  while (1) {
    write_line_to_shm((struct msg_handler_args*)args);
  }
  return NULL;
}

void process_messages(struct shm_storage* storage, struct interprocess_sync* reader_sync, struct interprocess_sync* writer_sync, struct shm_mtx* write_mtx) {
  struct msg_handler_args* write_args = malloc(sizeof(struct msg_handler_args));
  write_args->storage = storage;
  write_args->sync = writer_sync;  
  write_args->write_mtx = write_mtx;
  pthread_t write_thread;
  pthread_create(&write_thread, NULL, process_write, (void*)write_args);
  
  struct msg_handler_args* read_args = malloc(sizeof(struct msg_handler_args));
  read_args->storage = storage;
  read_args->sync = reader_sync;
  process_read((void*)read_args);
}

struct shm_storage* init_storage(char* shm_data_begin, size_t shm_data_size, char* shm_meta_begin, int flag) {
  struct shm_storage* storage = malloc(sizeof(struct shm_storage));
  storage->begin_space = shm_data_begin;
  storage->end_space = shm_data_begin + shm_data_size;
  storage->status = (int*)shm_meta_begin;
  storage->free_space = storage->begin_space;
  storage->free_space_size = shm_data_size - 2*(sizeof(char) + sizeof(size_t));
  if (flag == CREATE_FLAG) {
    *storage->status = WRITE;
    storage->first_record = NULL;
    *storage->record_count = 0;
  }
  return storage;
}

struct options init_default_options() {
  struct options console_options;
  console_options.shm_data_size = 1000;
  return console_options;
}

struct options parse_options(int argc, char **argv, int flag) {
  struct options console_options = init_default_options();
  if (flag == CREATE_FLAG) {
    int opt;
    while ((opt = getopt(argc, argv, "s:")) != -1) {
      switch (opt) {
      case 's': 
        console_options.shm_data_size = atoi(optarg);
        break;
      default:  
        error("unrecognized option");
      }
    } 
  } else if (getopt(argc, argv, "s") != -1) {
    error("unrecognized option");
  }
}

//pthread_cond_destroy(cv.pcond);
//pthread_condattr_destroy(&cv.attrcond); 

int main(int argc, char **argv) {
  struct options console_options;
  
  int shm_data_fd;
  char *shm_data_begin;
  
  int shm_meta_fd;
  char *shm_meta_begin;
  
  struct shm_storage* storage;

  sem_t *shm_mtx;

  if ((shm_mtx = sem_open(SHM_MTX, 0, 0, 0)) == SEM_FAILED) {
    if ((shm_mtx = sem_open(SHM_MTX, O_CREAT, 0660, 0)) == SEM_FAILED) {
      sys_error("sem_open, cannot create");
    }
  }

  if ((shm_data_fd = shm_open(SHM_DATA, OPEN_FLAG, OPEN_MODE)) == -1) {
    if ((shm_data_fd = shm_open(SHM_DATA, CREATE_FLAG, CREATE_MODE)) == -1) {
      sys_error("shm_data_open, cannot create");
    }

    console_options = parse_options(argc, argv, CREATE_FLAG);

    if (ftruncate(shm_data_fd, console_options.shm_data_size) == -1) {
      sys_error("shm_data_open, ftruncate");
    }
    if ((shm_meta_fd = shm_open(SHM_META, CREATE_FLAG, CREATE_MODE)) == -1) {
      sys_error("shm_meta_open, cannot create");
    }
    if (ftruncate(shm_meta_fd, SHM_META_SIZE) == -1) {
      sys_error("shm_meta_open, ftruncate");
    }
    shm_data_begin = shm_map_data_pointer(console_options.shm_data_size, shm_data_fd);
    memset(shm_data_begin, 0, console_options.shm_data_size);
    shm_meta_begin = shm_map_data_pointer(SHM_META_SIZE, shm_meta_fd);
    memset(shm_meta_begin, 0, SHM_META_SIZE);
    if (sem_post(shm_mtx) == -1) {
      sys_error("sem_post, shm_mtx");
    }
    struct interprocess_sync* sync1 = shm_init_sync(RW_CV1, RW_MTX1, READ1, CREATE_FLAG, CREATE_MODE);
    struct interprocess_sync* sync2 = shm_init_sync(RW_CV2, RW_MTX2, READ2, CREATE_FLAG, CREATE_MODE);
    struct shm_mtx* w_mtx = shm_mtx_init(W_MTX, CREATE_FLAG, CREATE_MODE);
    storage = init_storage(shm_data_begin, console_options.shm_data_size, shm_meta_begin, CREATE_FLAG);
    process_messages(storage, sync1, sync2, w_mtx);
  }

  // second process

  console_options = parse_options(argc, argv, OPEN_FLAG);
  if ((shm_meta_fd = shm_open(SHM_META, OPEN_FLAG, OPEN_MODE)) == -1) {
    sys_error("shm_meta_open, cannot open");
  }
  shm_meta_begin = shm_map_data_pointer(SHM_META_SIZE, shm_meta_fd);
  struct interprocess_sync* sync1 = shm_init_sync(RW_CV1, RW_MTX1, READ1, OPEN_FLAG, OPEN_MODE);
  struct interprocess_sync* sync2 = shm_init_sync(RW_CV2, RW_MTX2, READ2, OPEN_FLAG, OPEN_MODE);
  struct shm_mtx* w_mtx = shm_mtx_init(W_MTX, OPEN_FLAG, OPEN_MODE);
  shm_data_begin = shm_map_data_pointer(console_options.shm_data_size, shm_data_fd);
  storage = init_storage(shm_data_begin, console_options.shm_data_size, shm_meta_begin, OPEN_FLAG);
  process_messages(storage, sync2, sync1, w_mtx);
}