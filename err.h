#ifndef _ERR_
#define _ERR_

#include <stdio.h>

/* Wypisuje informacje o błędnym zakończeniu czegokolwiek i kończy działanie */
void syserr_(const char *fmt, ...);
void log_(const char *fmt, ...);
void log_first_(const char *fmt, ...);
void log_middle(const char *fmt, ...);
void log_last(const char *fmt, ...);

#define log(...) do { fprintf(stderr, "[%s:%d] ", __FUNCTION__, __LINE__); log_(__VA_ARGS__); } while(0)
#define log_first(...) do { fprintf(stderr, "[%s:%d] ", __FUNCTION__, __LINE__); log_first_(__VA_ARGS__); } while(0)
#define syserr(...) do { fprintf(stderr, "[%s:%d] ", __FUNCTION__, __LINE__); syserr_(__VA_ARGS__); } while(0)

#endif // _ERR_
