#include <unistd.h>

int main() {
    const char msg[] =
        "\n"
        "========================\n"
        " Bharat-linux CLI v0.1 \n"
        "========================\n";

    write(1, msg, sizeof(msg)-1);

    while(1) {
        pause();
    }

    return 0;
}
