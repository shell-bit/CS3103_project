CS3103 Operating Systems Project

Start with CS3103_Project_Specification.docx. It defines the assignment and
grading requirements. If a sample or starter differs from it, follow the
specification. The supplied examples add no grading requirements.

Package contents
  CS3103_Project_Specification.docx  Assignment and grading tables
  problem1.c                       Q1 student starter
  problem2.c                       Original Q2 starter
  problem2/                        Completed Q2 code, tests and report
  Makefile                         Linux build commands
  samples/                         Small input/output examples
  test_cases/q1/                   Macro configurations and testing notes
  test_cases/q2/                   Instructor-provided Q2 command scripts
  test_cases/q3/                   Scheduling input files

The Q1 starter prints TODO diagnostics and returns a failure status because
verification has not been implemented. The completed Q2 source is
problem2/problem2.c.

Q3 has no supplied source skeleton, as stated in the specification. Create
problem3.c and implement the required signatures from the document. MLFQ is
optional. No helper module is needed to compile the supplied Q1/Q2 starters.

Build on Linux
  make

Initially this builds problem1 and minifs. Once problem3.c exists, make also
builds problem3. Update the Makefile if you add source files or headers.
The compiler warning flags in this example are build aids.

Equivalent individual build commands:
  gcc -std=gnu11 -Wall -Wextra -pthread problem1.c -o problem1
  gcc -std=gnu11 -Wall -Wextra problem2/problem2.c -o minifs
  gcc -std=c11 -Wall -Wextra problem3.c -o problem3

Run Q1
  ./problem1

Edit the six macros in problem1.c and recompile to change the configuration.
Q1 does not read the old configuration files or command-line parameters.
See test_cases/q1/README.txt for the macro mapping and sample settings.

Run Q2 from the repository root
  make minifs
  ./minifs

Example commands:
  format
  create hello.txt
  write hello.txt "Operating Systems"
  read hello.txt
  exit

The surrounding double quotes are not stored as file content. ls takes no
argument and lists the current directory. To run a test from the repository root:
  ./minifs < problem2/test_cases/case1_basic.txt

See problem2/test_cases/README.txt for expected results, the two-run
persistence test, and commands to use when working inside problem2/.

The simulator stores its state in virtual_disk.bin, a 1 MiB disk with
4096 blocks of 256 bytes. Block 0 contains the superblock, blocks 1-64
contain the file allocation table, blocks 65-96 contain the 128 metadata
records, and blocks 97-4095 contain file data. The 97 reserved metadata
blocks count as used; the data region has 3999 blocks.

Run Q3 after implementing it
  ./problem3 test_cases/q3/case1.txt

Each supplied input starts with the header shown in the specification.
One invocation runs FCFS, SJF, preemptive Priority, RR q=3, and RR q=6 in that
order. There is no extra quantum argument. See test_cases/q3/README.txt.

Testing and submission
The samples illustrate the written requirements; they are not an exhaustive
test suite. Follow each problem's testing and report sections, and the common
submission section. Submit your completed sources, build instructions, tests,
report and executables under the names specified in the document.
See samples/README.txt for worked input/output examples for all three problems.
