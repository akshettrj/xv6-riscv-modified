#include "../kernel/types.h"
#include "user.h"

int
main(int argc, char **argv)
{
  if (argc < 3)
  {
    fprintf(2, "Usage: strace mask command [args]\n");
    exit(1);
  }

  int fork_ret = fork();

  if (fork_ret < 0)
  {
    fprintf(2, "Failed to fork a process");
    exit(1);
  }
  else if (fork_ret == 0)
  {
    if (trace(atoi(argv[1])) != 0)
    {
      fprintf(2, "Failed to start trace\n");
      exit(1);
    }
    exec(argv[2], argv+2);
    fprintf(2, "Failed to execute the command %s\n", argv[2]);
    exit(1);
  }
  else
  {
    wait(0);
  }

  exit(0);

}
