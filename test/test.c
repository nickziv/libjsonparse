#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <jsonparse.h>

char *
read_file(char *path, size_t *sz)
{
	int fd = openat(AT_FDCWD, path, O_RDONLY);
	if (fd < 0) {
		return (NULL);
	}
	struct stat buffer;
	int status = fstat(fd, &buffer);
	*sz = buffer.st_size;

	char *data = mmap(0, *sz, PROT_READ, MAP_PRIVATE, fd, 0);
	return (data);
}

int
main(int ac, char **av)
{
	char *file = NULL;
	file = av[1];

	size_t sz = 0;
	char *in = read_file(file, &sz);

	jsp_ast_t *ast = jsp_parse(in, sz);
}
