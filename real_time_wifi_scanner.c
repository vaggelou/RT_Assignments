/* 
 * REAL-TIME WiFi SCANNER
 *
 * Author: Angelou Evangelos
 * Date: 02-10-2017
 *
 */

#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#define NSEC_PER_SEC (1000000000ul)
#define QUEUESIZE 50
#define SSID_LEN 50
#define MAX_SSIDS 100

struct timespec task_timer;
long long int samplePeriod;
char ssids[MAX_SSIDS][SSID_LEN];
struct timespec timestamps[MAX_SSIDS];
struct timespec write_timestamps[MAX_SSIDS];
struct timespec latencies[MAX_SSIDS];
int num_timestamps;
int ssid_tail = 0;

typedef struct {
  char ssid_buf[QUEUESIZE][SSID_LEN];
  struct timespec time_buf[QUEUESIZE];
  long head, tail;
  int full, empty;
  pthread_mutex_t *mut;
  pthread_cond_t *notFull, *notEmpty;
} queue;

queue *queueInit(void)
{
  queue *q;

  q = (queue *)malloc(sizeof(queue));
  if (q == NULL) return (NULL);

  q->empty = 1;
  q->full = 0;
  q->head = 0;
  q->tail = 0;
  q->mut = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
  pthread_mutex_init(q->mut, NULL);
  q->notFull = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
  pthread_cond_init(q->notFull, NULL);
  q->notEmpty = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
  pthread_cond_init(q->notEmpty, NULL);
	
  return (q);
}

void queueDelete(queue *q)
{
  pthread_mutex_destroy(q->mut);
  free(q->mut);	
  pthread_cond_destroy(q->notFull);
  free(q->notFull);
  pthread_cond_destroy(q->notEmpty);
  free(q->notEmpty);
  free(q);
}

void queueAdd(queue *q, char *in, struct timespec tm)
{
  strcpy(q->ssid_buf[q->tail], in);
  q->time_buf[q->tail] = tm;
  q->tail++;
  if (q->tail == QUEUESIZE)
    q->tail = 0;
  if (q->tail == q->head)
    q->full = 1;
  q->empty = 0;

  return;
}

void queueDel(queue *q, char *out, struct timespec *tm)
{
  strcpy(out, q->ssid_buf[q->head]);
  *tm = q->time_buf[q->head];

  q->head++;
  if (q->head == QUEUESIZE)
    q->head = 0;
  if (q->head == q->tail)
    q->empty = 1;
  q->full = 0;

  return;
}

void writeToCSV(void)
{
  int i, j;
  char *pos;

  FILE *file = fopen("scan_results.csv", "w");

  if (file != NULL)
  {
    fprintf(file, "SSID,scan_timestamps(secs),scan_timestamps(nsecs),");
    fprintf(file, "write_timestamps(secs),write_timestamps(nsecs),");
    fprintf(file, "latency(secs),latency(nsecs)\n");

    for (i = 0; i < ssid_tail; i++)
    {
      if((pos=strchr(ssids[i], '\n')) != NULL)
        *pos = '\0';
      fprintf(file, "%s,", ssids[i]);
      fprintf(file, "%ld,%ld,", (long int)timestamps[i].tv_sec, (long int)timestamps[i].tv_nsec);
      fprintf(file, "%ld,%ld,", (long int)write_timestamps[i].tv_sec, (long int)write_timestamps[i].tv_nsec);
      fprintf(file, "%ld,%ld", (long int)latencies[i].tv_sec, (long int)latencies[i].tv_nsec);
      fprintf(file, "\n");
    }

    fclose(file);
  }
}

void *producer_thread_function(void *q)
{
  FILE *fp;
  char ssid[SSID_LEN];
  queue *fifo;
  int samp_sec;
  long int samp_nsec;
  struct timespec timestamp;
  fifo = (queue *)q;

  clock_gettime(CLOCK_MONOTONIC, &task_timer);

  while(1)
  {
    /* As the sample period is in nanoseconds, we divide it by the
    number of nanoseconds in a second (10^9) in order to extract 
    the seconds value of the sample period */
    samp_sec = samplePeriod / NSEC_PER_SEC;
    /* Then we calculate the mod of the division between the sample 
      period and the number of nanoseconds in a second (10^9) in 
      order to get the fraction of the second in nanoseconds */
    samp_nsec = samplePeriod % NSEC_PER_SEC;
    // Update the absolute time
    task_timer.tv_sec += samp_sec;
    task_timer.tv_nsec += samp_nsec;
    //Check if the nanoseconds value overflowed
    if(task_timer.tv_nsec >= NSEC_PER_SEC)
    {
      task_timer.tv_nsec -= NSEC_PER_SEC;
      task_timer.tv_sec++;
    }

    /* Open the command for reading. */
    fp = popen("sudo ./ssid_scan.sh", "r");
    if(fp == NULL)
    {
      printf("Failed to run scan_ssid bash script\n");
      exit(1);
    }

    pthread_mutex_lock(fifo->mut);
    while(fifo->full)
    {
      printf("producer: queue FULL.\n");
      pthread_cond_wait(fifo->notFull, fifo->mut);
    }

    //START OF SSID SCAN
    /* Read the output a line at a time - output it. */
    // clock_gettime(CLOCK_MONOTONIC, &timestamp);
    while(fgets(ssid, sizeof(ssid) - 1, fp) != NULL)
    {
      clock_gettime(CLOCK_MONOTONIC, &timestamp);
      queueAdd(fifo, ssid, timestamp);
    }
    /* close */
    pclose(fp);
    //END OF SSID SCAN
    
    pthread_mutex_unlock (fifo->mut);
    pthread_cond_signal (fifo->notEmpty);

    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &task_timer, NULL);
  }

  return (NULL);
}

void *consumer_thread_function(void *q)
{
  queue *fifo;
  char ssid[SSID_LEN];
  struct timespec timestamp;
  long long int timestamp_in_nano, write_timestamps_in_nano;
  long long int temp;
  int ssid_found = 0;
  int i, ssid_index;
  char *pos;

  fifo = (queue *)q;

  while(1)
  {
    pthread_mutex_lock (fifo->mut);
    while (fifo->empty) {
      printf ("consumer: queue EMPTY.\n");
      pthread_cond_wait (fifo->notEmpty, fifo->mut);
    }
    queueDel(fifo, ssid, &timestamp);

    if((pos=strchr(ssid, '\n')) != NULL) //to eliminate the \n character
      *pos = '\0';
    for(i = 0; i < ssid_tail; i++)
    {
      if(!strcmp(ssids[i], ssid)) //if ssid is already in the array
      {
        ssid_found = 1; //set ssid_found flag 
        ssid_index = i;
      }
    }
    if(ssid_found)
    {
      //if ssid is in the array, put its timestamp into the timestamp array
      timestamps[ssid_index] = timestamp;
      clock_gettime(CLOCK_MONOTONIC, &write_timestamps[ssid_index]);
      timestamp_in_nano = timestamp.tv_sec*NSEC_PER_SEC + timestamp.tv_nsec;
      write_timestamps_in_nano = write_timestamps[ssid_index].tv_sec*NSEC_PER_SEC + write_timestamps[ssid_index].tv_nsec;
      temp = (long int)(write_timestamps_in_nano - timestamp_in_nano);
      latencies[ssid_index].tv_sec = temp / NSEC_PER_SEC;
      latencies[ssid_index].tv_nsec = temp % NSEC_PER_SEC;
      ssid_found = 0;
    }
    else //if ssid is scanned for the first time
    {
      //put it and its timestamp into their respective arrays
      strcpy(ssids[ssid_tail], ssid);
      timestamps[ssid_tail] = timestamp;
      clock_gettime(CLOCK_MONOTONIC, &write_timestamps[ssid_tail]);
      timestamp_in_nano = timestamp.tv_sec*NSEC_PER_SEC + timestamp.tv_nsec;
      write_timestamps_in_nano = write_timestamps[ssid_tail].tv_sec*NSEC_PER_SEC + write_timestamps[ssid_tail].tv_nsec;
      temp = (long int)(write_timestamps_in_nano - timestamp_in_nano);
      latencies[ssid_tail].tv_sec = temp / NSEC_PER_SEC;
      latencies[ssid_tail].tv_nsec = temp % NSEC_PER_SEC;
      ssid_tail++;
      if(ssid_tail >= MAX_SSIDS)
      {
        //if ssid_tail=max number of ssids, start write from the start
        ssid_tail = 0;
      }
    }

    pthread_mutex_unlock (fifo->mut);
    pthread_cond_signal (fifo->notFull);

    //write ssid and timestamp arrays to file
    writeToCSV();
  }

  return (NULL);
}

int main(int argc, char* argv[])
{
    struct sched_param param_prod, param_cons;
    pthread_attr_t attr_prod, attr_cons;
    pthread_t thread_prod, thread_cons;
    queue *fifo;
    int ret = 1;
    samplePeriod = (long long int)(atof(argv[1]) * NSEC_PER_SEC); //sample period in nanoseconds

    fifo = queueInit();
    if(fifo ==  NULL)
    {
      fprintf (stderr, "main: Queue Init failed.\n");
      exit(1);
    }

    //Lock memory
    if(mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        printf("Memory lock failed: %m\n");
        exit(-2);
    }

    /* Initialize pthread attributes (default values) */
    //Producer thread
    ret = pthread_attr_init(&attr_prod);
    if (ret) {
        printf("producer pthread attributes initialization failed\n");
        return ret;
    }
    //Consumer thread
    ret = pthread_attr_init(&attr_cons);
    if (ret) {
        printf("consumer pthread attributes initialization failed\n");
        return ret;
    }

    /* Set scheduler policy and priority of pthread */
    //Producer thread
    ret = pthread_attr_getschedparam(&attr_prod, &param_prod);
    if (ret) {
        printf("get of producer pthread parameters failed\n");
        return ret;
    }
    param_prod.sched_priority = sched_get_priority_max(SCHED_RR);
    ret = pthread_attr_setschedpolicy(&attr_prod, SCHED_RR);
    if (ret) {
        printf("producer pthread schedule policy set failed\n");
        return ret;
    }
    ret = pthread_attr_setschedparam(&attr_prod, &param_prod);
    if (ret) {
        printf("set of producer pthread parameters failed\n");
        return ret;
    }
    //Consumer thread
    ret = pthread_attr_getschedparam(&attr_cons, &param_cons);
    if (ret) {
        printf("get of consumer pthread parameters failed\n");
        return ret;
    }
    param_cons.sched_priority = sched_get_priority_max(SCHED_RR);
    ret = pthread_attr_setschedpolicy(&attr_cons, SCHED_RR);
    if (ret) {
        printf("consumer pthread schedule policy set failed\n");
        return ret;
    }
    ret = pthread_attr_setschedparam(&attr_cons, &param_cons);
    if (ret) {
        printf("set of consumer pthread parameters failed\n");
        return ret;
    }

    /* Create a pthread with specified attributes */
    //Producer thread
    ret = pthread_create(&thread_prod, &attr_prod, producer_thread_function, fifo);
    if (ret) {
        printf("create of producer pthread failed\n");
        return ret;
    }
    //Consumer thread
    ret = pthread_create(&thread_cons, &attr_cons, consumer_thread_function, fifo);
    if (ret) {
        printf("create of consumer pthread failed\n");
        return ret;
    }
    pthread_setschedparam(thread_prod, SCHED_RR, &param_prod);
    pthread_setschedparam(thread_cons, SCHED_RR, &param_cons);

    /* Join the thread and wait until it is done */
    //Producer thread
    ret = pthread_join(thread_prod, NULL);
    if (ret) printf("join producer pthread failed: %m\n");
    //Consumer thread
    ret = pthread_join(thread_cons, NULL);
    if (ret) printf("join consumer pthread failed: %m\n");

    /* Destroy the pthread attribute object */
    //Producer thread
    ret = pthread_attr_destroy(&attr_prod);
    if (ret) {
        printf("producer pthread attribute destruction failed\n");
        return ret;
    }
    //Consumer thread
    ret = pthread_attr_destroy(&attr_cons);
    if (ret) {
        printf("consumer pthread attribute destruction failed\n");
        return ret;
    }

    return ret;
}