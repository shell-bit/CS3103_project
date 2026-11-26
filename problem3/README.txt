CS3103 Problem 3 — Scheduling

Build and run from this directory

    make -B problem3
    ./problem3 test_cases/q3/case1.txt
    ./problem3 test_cases/q3/case1.txt --mlfq
    make test

The bundled problem3 is a statically linked Linux x86-64 executable.
The source builds with a C11 compiler and POSIX getline. The supplied
Makefile defaults to gcc. The test script uses Python 3.9 or later.
Use make -B test CC=gcc to rebuild and verify on Linux.

The source implements the five required configurations in assignment order.
Passing --mlfq appends the optional sixth configuration. The parser accepts
the documented header and also accepts headerless rows. Job IDs must be
unique. Arrival times are nonnegative, bursts are positive and priorities
range from 1 (highest) to 5 (lowest). At RR slice boundaries, jobs arriving
at that instant enter the ready queue before the expired job is requeued.

The report contains the Problem 3 report section. Keep the barchart's figure alongside
it so the average waiting time chart renders. The report gives hand
calculations and test results. Run test_q3.py to reproduce the automated checks.
test_cases/q3/ contains the course inputs and the worked sample.
