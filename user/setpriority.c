#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  if (argc < 3)
  {
    fprintf(2, "Usage: setpriority new_priority pid\n");
    exit(1);
  }

  int new_priority = atoi(argv[1]);
  int pid = atoi(argv[2]);

  int ret = set_priority(new_priority, pid);

  if (ret != 0)
  {
    fprintf(2, "Failed to set the priority\n");
    exit(1);
  }

  exit(0);

}
