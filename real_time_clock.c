/* 
 * REAL-TIME CLOCK
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

static int numOfSamples;    //Number of samples to acquire
static long long int samplePeriod;  //Sample period (in nanoseconds)

/* @brief Function to create a .csv file with the timestamps. */
void createDataCSV(char *filename, struct timespec *tm_array, long int *drift)
{
	FILE *fp;
	int i;

	printf("Creating %s.csv file\n", filename);

	filename = strcat(filename, ".csv");

	//Open the .csv file.
	fp = fopen(filename, "w+");

    //Write the timestamps into the file.
    fprintf(fp, "index,timestamps(sec),timestamp(nsec),drift(nsec)\n");
	for(i = 0; i < numOfSamples; i++)
	{
		fprintf(fp, "%d,%ld,%09ld,%ld\n", i, tm_array[i].tv_sec, tm_array[i].tv_nsec, drift[i]);
    }

	//Close the .csv file.
	fclose(fp);

	printf("%s file created\n", filename);
	printf("\n");
}

/* @brief Main thread function. */
void *main_thread_function(void *ptr)
{
    // struct timespec timestamp;
    struct timespec ground_truth[numOfSamples];
    struct timespec deadline;
    struct timespec *timeArray;
    char filename[20];
    int samp_sec;
    long int samp_nsec;
    int i;
    int retval;
    long int driftArray[numOfSamples];
    timeArray = (struct timespec *)ptr;

    //Get clock reference time
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    printf("#----Sampling begin----#\n");
    for(i = 0; i < numOfSamples; i++)
    {
        ground_truth[i].tv_sec = deadline.tv_sec;
        ground_truth[i].tv_nsec = deadline.tv_nsec;
        //Get a time sample and store into the samples array
        clock_gettime(CLOCK_MONOTONIC, &timeArray[i]);
        driftArray[i] = (timeArray[i].tv_sec*NSEC_PER_SEC + timeArray[i].tv_nsec) - (ground_truth[i].tv_sec*NSEC_PER_SEC + ground_truth[i].tv_nsec);
        /* As the sample period is in nanoseconds, we divide it by the
           number of nanoseconds in a second (10^9) in order to extract 
           the seconds value of the sample period */
        samp_sec = samplePeriod / NSEC_PER_SEC;
        /* Then we calculate the mod of the division between the sample 
           period and the number of nanoseconds in a second (10^9) in 
           order to get the fraction of the second in nanoseconds */
        samp_nsec = samplePeriod % NSEC_PER_SEC;
        // Update the absolute time
        deadline.tv_sec += samp_sec;
        deadline.tv_nsec += samp_nsec;
        //Check if the nanoseconds value overflowed
        if(deadline.tv_nsec >= NSEC_PER_SEC)
        {
            deadline.tv_nsec -= NSEC_PER_SEC;
            deadline.tv_sec++;
        }
        
        retval = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        if(retval != 0)
        {
            printf("Sleep failed!\n");
            exit(1);
        }
    }
    printf("#----Sampling end----#\n");

    //Print the timestamps acquired
    for(i = 0; i < numOfSamples; i++)
    {
        printf("Timestamp#%i = %ld.%09ld\n", i, (long int)timeArray[i].tv_sec, (long int)timeArray[i].tv_nsec);
    }
    for(i = 0; i < numOfSamples; i++)
    {
        printf("Drift#%i = %ld\n", i, (long int)driftArray[i]);
    }

    //Enter desired file name from the terminal.
	printf("Enter file name to save the data: ");
	scanf("%s", filename);

	//Create .csv file.
	createDataCSV(filename, timeArray, driftArray);
}
 
int main(int argc, char* argv[])
{
    struct sched_param param;
    pthread_attr_t attr;
    pthread_t thread;
    int ret = 1;
    numOfSamples = atoi(argv[1]);
    samplePeriod = (long long int)(atof(argv[2]) * NSEC_PER_SEC); //sample period in nanoseconds

    //Allocate memory for the array of the time samples
    struct timespec *timeArray = (struct timespec *)malloc(numOfSamples * sizeof(struct timespec));

    //Lock memory
    if(mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        printf("Memory lock failed: %m\n");
        exit(-2);
    }

    //Initialize pthread attributes (default values)
    ret = pthread_attr_init(&attr);
    if (ret) {
        printf("pthread attributes initialization failed\n");
        return ret;
    }

    //Set scheduler policy and priority of pthread
    ret = pthread_attr_getschedparam(&attr, &param);
    if (ret) {
        printf("Get of pthread parameters failed\n");
        return ret;
    }
    param.sched_priority = sched_get_priority_max(SCHED_RR);
    ret = pthread_attr_setschedpolicy(&attr, SCHED_RR);
    if (ret) {
        printf("pthread schedule policy set failed\n");
        return ret;
    }
    ret = pthread_attr_setschedparam(&attr, &param);
    if (ret) {
        printf("Set of pthread parameters failed\n");
        return ret;
    }

    //Create a pthread with specified attributes
    ret = pthread_create(&thread, &attr, main_thread_function, (void *)timeArray);
    if (ret) {
        printf("create pthread failed\n");
        return ret;
    }

    //Join the thread and wait until it is done
    ret = pthread_join(thread, NULL);
    if (ret)
        printf("join pthread failed: %m\n");

    //Destroy the pthread attribute object
    ret = pthread_attr_destroy(&attr);
    if (ret) {
        printf("pthread attribute destruction failed\n");
        return ret;
    }

    return ret;
}
