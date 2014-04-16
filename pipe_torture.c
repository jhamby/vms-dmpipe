/* pipe_torture.c
 * 
 * Craig A. Berry   5 January 2014
 *
 * Test various IPC scenarios using pipes via popen().  It's inspired by a Perl
 * implementation posted to vmsperl@perl.org on 25 April 2002 by Chuck Lane,
 * Message-Id: <020425091835.ed1bb@DUPHY4.Physics.Drexel.Edu>.
 * 
 * This program should work the same on any system that supports pipes, but it's 
 * specifically intended to reveal deficiencies in the traditional mailbox-based
 * pipe implementation on OpenVMS, notably:
 *
 *     1.) Hanging when the other end has gone away or stopped responding.
 *     2.) Non-standard inheritance of standard streams (e.g. a child writer getting its
 *         stderr mapped to its stdout when no such mapping has been requested).
 *     3.) Introducing spurious record boundaries when a write is flushed or a mailbox
 *         buffer fills up.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <unixio.h>
#include "dmpipe.h"	/* wrap function */

struct test_data {
    int num_child_ops;
    int num_parent_ops;
    int pre_sleep;
    int post_sleep;
    char frequent_flusher; 
    char mode[2];
    char msg[132];
};

#define NUM_TESTS 11

struct test_data tests[NUM_TESTS] = {
    {  10,  10,  0,  10, 1, "w",   /* 1 */
      "child sleeps after parent closes; should see 10 writes, then 10 reads" },
    {  10,  10,  0,   0, 0, "w",
      "child exits when finished; should see 10 writes, then 10 reads" },
    {  10,   5,  0,   5, 0, "w",
      "child writes 10, parent reads 5 and closes; should see 10 writes, 5 reads" },
    {  10,   5,  0,   5, 1, "w",
      "child writes/flushes 10, parent reads 5 and closes; should see 6 writes, 5 reads" },
    {   5,  10,  0,   0, 0, "w",  /* 5 */
      "child writes 5, parent reads 10; should see 5 writes, 5 reads, 5 EOFs" },
    { 100, 100,  0,   0, 0, "w",
      "should see 100 writes, then 100 reads" },
    {  10,  10,  0,   0, 0, "r",
      "parent writes 10, child reads 10; should see 10 writes, 10 reads" },
    {   6,   5,  0,   0, 0, "r",
      "parent writes 5, child reads 6; should see 5 writes, 5 reads, 1 EOF" },
    {  10,   5,  2,   0, 0, "r",
      "parent writes 5, child reads 10; should see 5 writes, 5 reads, 5 EOFs" },
    {   0,   5,  0,   0, 0, "r",   /* 11 */
     "parent writes 5, child reads none; should see 5 writes only" },
    {   5,  10,  0,   0, 0, "r",
      "parent writes 10, child reads 5 and exits; should see 10 writes, 5 reads" }
};

#define READBUF_SIZE 256

int main(int argc, char *argv[]) {
    FILE *pipe_fp, *infile;
    char readbuf[READBUF_SIZE];
    char cmd[256];
    char grandparent = 0, parent = 0, child = 0;
    int i;
    char cmd_prefix[5];
#ifdef __VMS
    strcpy(cmd_prefix, "mcr ");
#else
    strcpy(cmd_prefix, "");
#endif

    switch (argc) {
        case 1 :
           grandparent = 1;
           break;
        case 3 :
           if (strncmp(argv[1], "parent", 6) == 0) {
               parent = 1;
               break;
           }
        case 8 :
           if (strncmp(argv[1], "child", 5) == 0) {
               child = 1;
               break;
           }
        default :
           fprintf(stderr, "Usage:  pipe_torture [parent testnum | child testnum args]\n");
           exit(1);
    }

    if (grandparent) {
        for (i = 1; i <= NUM_TESTS; i++) {
            sprintf(cmd, "%s%s parent %d", cmd_prefix, argv[0], i);
            if (( pipe_fp = popen(cmd, "r")) == NULL) {
                    perror("grandparent popen");
                    exit(1);
            }
            while(fgets(readbuf, READBUF_SIZE, pipe_fp))
                printf("%s", readbuf);
            fflush(pipe_fp);
            pclose(pipe_fp);
        }

        return(0);
    }
    else if (parent) {
        int testnum = atoi(argv[2]);
        char parent_mode[2];

        fprintf(stderr, "\nTEST %d: %s\n", testnum, tests[testnum-1].msg);
        sprintf(cmd, "%s%s child %d %s %d %d %d %d",
                cmd_prefix, 
                argv[0],
                testnum,
                tests[testnum-1].mode, 
                tests[testnum-1].pre_sleep,
                tests[testnum-1].num_child_ops,
                tests[testnum-1].pre_sleep,
                tests[testnum-1].frequent_flusher);

        /* Parent reads when child writes and vice versa. */
        if (*tests[testnum-1].mode == 'w')
            strcpy(parent_mode, "r");
        else
            strcpy(parent_mode, "w");

        if (( pipe_fp = popen(cmd, parent_mode)) == NULL) {
            perror("parent popen");
            exit(1);
        }
        if (parent_mode[0] == 'r') {
            for (i = 1; i <= tests[testnum-1].num_parent_ops; i++) {
                if (!fgets(readbuf, READBUF_SIZE, pipe_fp))
                    strcpy(readbuf, "<EOF>\n");
                printf("parent read from child (%d/%d): %s",
                       i,
                       tests[testnum-1].num_parent_ops,
                       readbuf);
            }
        }
        else if (parent_mode[0] == 'w') {
            for (i = 1; i <= tests[testnum-1].num_parent_ops; i++) {
                fprintf(pipe_fp, "this is parent, writing line %d/%d to child/test%d\n",
                        i,
                        tests[testnum-1].num_parent_ops,
                        testnum);
                fprintf(stderr, "this is parent, writing line %d/%d to child/test%d\n",
                        i,
                        tests[testnum-1].num_parent_ops,
                        testnum);
            }
        }
        pclose(pipe_fp);
        
    }
    else if (child) {
        int testnum     = atoi(argv[2]);
        char *mode      = argv[3];
        int pre_sleep   = atoi(argv[4]);
        int num_ops     = atoi(argv[5]);
        int post_sleep  = atoi(argv[6]);
        char frequent_flusher = (char)atoi(argv[7]); char buffer[400];
        int j;

        if (pre_sleep)
            sleep(pre_sleep);

        for (i = 1; i <= num_ops; i++) {
            if (*mode == 'r') {
                if (!fgets(readbuf, READBUF_SIZE, stdin))
                    strcpy(readbuf, "<EOF>\n");
                fprintf(stderr, "test%d read (%d/%d): %s",
                        testnum,
                        i,
                        num_ops,
                        readbuf);
            }
            else if (*mode == 'w') {
                fprintf(stderr, 
                        "test%d writing (%d/%d)....................\n",
                       testnum,
                       i,
                       num_ops);
                printf("test%d Writing [%d/%d]",
                       testnum,
                       i,
                       num_ops);
                /* Make sure we get no spurious record boundaries. */
                for (j = 0; j < 20; j++) {
                    printf(".");
                    if (frequent_flusher)
                        fflush(stdout);
                }
                printf("\n");
            }
        }
        if (post_sleep)
            sleep(post_sleep);
    }
}
