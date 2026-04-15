#include <stdio.h>
#include <string.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <command> [args]\n"
            "\n"
            "Commands:\n"
            "  status                       Live UPS status\n"
            "  events [--limit N]           Recent events\n"
            "  cmd <action>                 UPS commands\n"
            "  config dump|get|set          UPS config registers\n"
            "  app config get|set           App configuration\n"
            "  weather status               Weather assessment\n"
            "  shutdown test <target>       Test shutdown target\n"
            "\n"
            "The CLI connects to the airies-upsd daemon via unix socket.\n"
            "The daemon must be running.\n",
            prog);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "airies-ups: not yet implemented\n");
    return 1;
}
