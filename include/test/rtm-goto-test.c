/* Version of rtm-test.c using the goto variants.
 * This exposes the abort handler control flow directly.
 * Requires specific compilers.
 */
#include "rtm-goto.h"
#include <stdio.h>

/* Requires TSX support in the CPU */

int main(void)
{
	unsigned status;

	XBEGIN(abort);
	if (XTEST())
		XABORT(1);
	XEND();
	return 0;

	/* Abort comes here */
	XFAIL_STATUS(abort, status);
	printf("aborted %x, %d", status, XABORT_CODE(status));
	return 0;
}
