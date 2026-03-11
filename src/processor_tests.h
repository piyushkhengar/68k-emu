/*
 * ProcessorTests harness: run SingleStepTests/680x0 JSON test suite.
 * Usage: ./68k-emu --processor-tests path/to/68000/v1
 */

#ifndef PROCESSOR_TESTS_H
#define PROCESSOR_TESTS_H

/* Run ProcessorTests from directory. filter=NULL runs all; otherwise only files
 * whose basename (without .json.gz) contains filter. Returns 0 if all pass, 1 if any fail. */
int run_processor_tests(const char *dir, const char *filter);

#endif /* PROCESSOR_TESTS_H */
