/* snarf9.c: /dev/snarf device I/O on 9front. Impure.
 * The device is opened fresh for every operation: rio applies a write at close
 * and a long-lived fd would read stale data. */
#include "compat.h"
#include <draw.h>
#include <cell.h>
#include "coil.h"
#include "host.h"
#include "shell.h"

enum { Snarfchunk = 8192 };

int
s9_snarfget(Host*, char **data, Pos *n, char *err, int nerr)
{
	int fd;
	char *buf, *nb;
	long k;
	vlong tot, cap;

	fd = open("/dev/snarf", OREAD);
	if(fd < 0){
		snprint(err, nerr, "%r");
		return -1;
	}
	cap = Snarfchunk;
	tot = 0;
	buf = malloc(cap + 1);
	if(buf == nil){
		close(fd);
		snprint(err, nerr, "out of memory");
		return -1;
	}
	for(;;){
		if(tot == cap){
			if(cap >= Snarfread){
				free(buf);
				close(fd);
				snprint(err, nerr, "snarf too large");
				return -1;
			}
			cap *= 2;
			nb = realloc(buf, cap + 1);
			if(nb == nil){
				free(buf);
				close(fd);
				snprint(err, nerr, "out of memory");
				return -1;
			}
			buf = nb;
		}
		k = read(fd, buf + tot, cap - tot);
		if(k < 0){
			snprint(err, nerr, "%r");
			free(buf);
			close(fd);
			return -1;
		}
		if(k == 0)
			break;
		tot += k;
		if(tot > Snarfread){
			free(buf);
			close(fd);
			snprint(err, nerr, "snarf too large");
			return -1;
		}
	}
	close(fd);
	buf[tot] = '\0';
	*data = buf;
	*n = (Pos)tot;
	return 0;
}

int
s9_snarfput(Host*, char *p, Pos n, char *err, int nerr)
{
	int fd;
	Pos off;
	long k, want;

	fd = open("/dev/snarf", OWRITE);	/* open for write resets rio's pending buffer */
	if(fd < 0){
		snprint(err, nerr, "%r");
		return -1;
	}
	for(off = 0; off < n; off += k){
		want = n - off > Snarfchunk ? Snarfchunk : (long)(n - off);
		k = write(fd, p + off, want);
		if(k != want){
			snprint(err, nerr, "%r");
			close(fd);
			return -1;
		}
	}
	if(close(fd) < 0){
		snprint(err, nerr, "%r");
		return -1;
	}
	return 0;
}
