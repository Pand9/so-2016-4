#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signal.h>
#include <unistd.h>

#include "kom.h"
#include "err.h"

#ifdef NO_LOGGING
#  undef log
#  define log(...)
#  undef log_first
#  define log_first(...)
#  define log_middle(...)
#  define log_last(...)
#endif // NO_LOGGING

#define SUM_INDICES_LIMIT 10

size_t waittime;
bool interrupt;

#define I_BUF_LEN 256
char i_buf[I_BUF_LEN];

static
int communicate(const struct array_msg_from_client *msg,
		struct array_msg_from_server *resp) {
	if (1 == client_communicate(msg, resp)) {
		log("Interrupt or device removed.");
		return 1;
	}
	return 0;
}

static
void parse_ints(const char *str, size_t minc, size_t maxc, int *intc, int *intv) {
	for (size_t i = 0u; i < maxc; i++) {
		const char *const pstr = str;
		intv[i] = strtol(str, (char**) &str, 0);
		if (pstr == str) {
			if (intc) *intc = i;
			if (i < minc) {
				syserr("Got %d args, expected [%d,%d].", i, minc, maxc);
			}
			return;
		}
	}
	if (intc) *intc = maxc;
}

static
void sort(size_t count, int *nums) {
	for (size_t i = 0; i + 1 < count; i++) {
		for (size_t j = i + 1; j < count; j++) {
			if (nums[i] > nums[j]) {
				int tmp = nums[i];
				nums[i] = nums[j];
				nums[j] = tmp;
			}
		}
	}
}

static
void check_idx(int idx) {
	if (idx < 0)
		syserr("Incorrect idx %d", idx);
}

static
void prepare_idxs(size_t count, int *idxs) {
	for (size_t i = 0; i < count; i++)
		check_idx(idxs[i]);
	sort(count, idxs);
}

static
int run_getvalues(int count, const int *indices, int *out_values, _Bool lock, const int *rv_index) {
	struct array_msg_from_client msg = {
			.type = ArrayMsgtypeFromClientGetvalues,
			.pid = getpid(),
			.args.getvalues.count = count
	};
	if (rv_index) {
		msg.args.getvalues.rvlock = true;
		msg.args.getvalues.rv_index = *rv_index;
	}
	for (int i = 0; i < count; i++)
		msg.args.getvalues.indices[i] = indices[i];
	msg.args.getvalues.lock = lock;

	struct array_msg_from_server resp;
	if (1 == communicate(&msg, &resp))
		return 1;

	if (ArrayMsgtypeFromServerValues != resp.type)
		syserr("Wrong response from server.");

	if (count != resp.args.values.count)
		syserr("Wrong 'count' in 'values' response from server: %d.",
				resp.args.values.count);

	for (int i = 0; i < count; i++)
		out_values[i] = resp.args.values.values[i];
	return 0;
}

static
int run_setvalues(int count, const int *indices, const int *values) {
	struct array_msg_from_client msg = {
			.type = ArrayMsgtypeFromClientSetvalues,
			.pid = getpid(),
			.args.setvalues.count = count
	};
	for (int i = 0; i < count; i++) {
		msg.args.setvalues.indices[i] = indices[i];
		msg.args.setvalues.values[i] = values[i];
	}
	struct array_msg_from_server resp;
	if (1 == communicate(&msg, &resp))
		return 1;
	if (ArrayMsgtypeFromServerOk != resp.type)
		syserr("Wrong response from server.");
	return 0;
}

static void swap(int *v) {
	int tmp = v[0];
	v[0] = v[1];
	v[1] = tmp;
}

static
int do_swap() {
	assert(i_buf[0] == 'x');

	int idxs[2];
	parse_ints(&i_buf[2], 2, 2, NULL, idxs);
	log("do_swap(idxs[0]=%d, idxs[1]=%d) (possibly not sorted yet)", idxs[0], idxs[1]);
	prepare_idxs(2, idxs);

	int v[2];
	if (1 == run_getvalues(2, idxs, v, true, NULL))
		return 1;
	log("Old values: %d, %d", v[0], v[1]);
	sleep(waittime);

	swap(v);
	if (1 == run_setvalues(2, idxs, v))
		return 1;
	log("New values: %d, %d", v[0], v[1]);
	return 0;
}

static
int do_getvalue() {
	assert(i_buf[0] == 'r');

	int args[1];
	parse_ints(&i_buf[2], 1, 1, NULL, args);

	const int idx = args[0];
	log("do_getvalue(idx=%d)", idx);
	check_idx(idx);
	log("hello");
	int val;
	if (1 == run_getvalues(1, &idx, &val, false, NULL))
		return 1;
	log("Value read: %d.", val);
	printf(" %d", val);
	return 0;
}

static
int do_setvalue() {
	assert(i_buf[0] == 'w');

	int args[2];
	parse_ints(&i_buf[2], 2, 2, NULL, args);

	const int idx = args[0], val = args[1];
	log("do_setvalue(idx=%d, val=%d)", idx, val);
	check_idx(idx);

	if (1 == run_setvalues(1, &idx, &val))
		return 1;
	log("Value set: a[%d]=%d.", idx, val);
	return 0;
}

static
int do_sum() {
	assert(i_buf[0] == 's');

	int args[SUM_INDICES_LIMIT], argc;
	parse_ints(&i_buf[2], 2, SUM_INDICES_LIMIT, &argc, args);

	const int idx_dst = args[0];

	int *idx_src = &args[1];
	const int idx_src_cnt = argc - 1;
	log("do_sum(idx_dst=%d, [%d idx_srcs])", idx_dst, idx_src_cnt);
	check_idx(idx_dst);
	prepare_idxs(idx_src_cnt, idx_src);

	int sum = 0;
	{
		int vals[idx_src_cnt];
		if (1 == run_getvalues(idx_src_cnt, idx_src, vals, true, &idx_dst))
			return 1;
		for (int i = 0; i < idx_src_cnt; i++)
			sum += vals[i];
	}
	log("Sum computed: %d.", sum);
	sleep(waittime);
	if (1 == run_setvalues(1, &idx_dst, &sum))
		return 1;
	log("Sum set: a[%d]=%d.", idx_dst, sum);
	printf(" %d", sum);
	return 0;
}

static
void sigint_handler	(int sig) {
	interrupt = true;
}

static
void override_sigint() {
	struct sigaction sigact;
	sigact.sa_handler = sigint_handler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = SA_RESETHAND;
	if (-1 == sigaction(SIGINT, &sigact, 0))
		syserr("sigaction");
}

static
size_t parse_time(char *arg) {
	char *endptr;
	long ltime = strtol(arg, &endptr, 0);
	if (endptr == arg || *endptr != '\0' || ERANGE == errno)
		syserr("Cmdline arg conversion to int error: %d.", ltime);
	return (size_t) ltime;
}

int main(int argc, char **argv) {
	if (argc != 2)
		syserr("Bad args: expected <prog> <WAITTIME>");
	waittime = parse_time(argv[1]);

	override_sigint();

	client_init(getpid());
	fprintf(stderr, "Connection ready. x=swap, s=sum, r=get, w=set.\n");

	while (!interrupt && NULL != fgets(i_buf, I_BUF_LEN, stdin)) {
		if (i_buf[strlen(i_buf)-1] == '\n')
			i_buf[strlen(i_buf)-1] = '\0';
		fputs(i_buf, stdout);
		int res = 0;
		switch (i_buf[0]) {
		case 'r':
			res = do_getvalue();
			break;
		case 'w':
			res = do_setvalue();
			break;
		case 's':
			res = do_sum();
			break;
		case 'x':
			res = do_swap();
			break;
		default:
			if (isspace(i_buf[0]) || '\0' == i_buf[0])
				break;
			syserr("Unknown function type: '%c'.", i_buf[0]);
			return 1;
		}
		if (1 == res) {
			log("quitting, because server quit or some other signal");
			return 1;
		}
		printf("\n");
	}
	log("Cleaning up...");
	client_disconnect_server(getpid());
	return 0;
}
