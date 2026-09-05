/* ============================================================================
 *  tests/report.h -- send a test suite's full output to a file
 *
 *  Every suite writes its detailed report to results/<name>.txt and prints
 *  only a one-line verdict to the terminal, so a run stays readable and the
 *  evidence is still on disk for a report or a diff.
 *
 *      ./test_tlb_workflow                    -> results/test_tlb_workflow.txt
 *      ./test_tlb_workflow  my_report.txt     -> my_report.txt
 *      ./test_tlb_workflow  -                 -> the terminal, as before
 *
 *  Implementation note: stdout is redirected with freopen(), so the suites
 *  keep using plain printf() and nothing else had to change. The verdict goes
 *  to stderr, which is never redirected -- so it still reaches the terminal
 *  even when stdout is a file.
 * ==========================================================================*/

#ifndef TEST_REPORT_H
#define TEST_REPORT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char report_path_[512];

/* Redirect stdout. Pass "-" as argv[1] to keep output on the terminal. */
static const char *report_begin(int argc, char **argv, const char *dflt)
{
    const char *p = (argc > 1 && argv[1][0] != '\0') ? argv[1] : dflt;

    if (strcmp(p, "-") == 0) {
        report_path_[0] = '\0';
        return NULL;                     /* leave stdout alone */
    }

    /* Create results/ if the default path is being used. Failure is fine --
     * it usually just means the directory already exists. */
    mkdir("results", 0755);

    snprintf(report_path_, sizeof(report_path_), "%s", p);
    if (!freopen(report_path_, "w", stdout)) {
        fprintf(stderr, "cannot write %s: ", report_path_);
        perror(NULL);
        exit(2);
    }
    return report_path_;
}

/* Flush the report and print the one-line verdict to the terminal. */
static void report_end(const char *suite, const char *unit,
                       int total, int failed)
{
    fflush(stdout);
    fprintf(stderr, "%-22s %3d %-10s %d failed   %s%s\n",
            suite, total, unit, failed,
            failed ? "*** FAILURES ***  " : "",
            report_path_[0] ? report_path_ : "(terminal)");
}

#endif /* TEST_REPORT_H */
