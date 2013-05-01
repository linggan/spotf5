#include <stdio.h>
#include <string.h>

#include "f5.h"
#include "newcommon.h"
#include "strlcat.h"

char *
quality(char *prepend, int q)
{
    static char quality[1024];
    char stars[4];
    int i;

    for (i = 0; i < q && i < 3; i++)
        stars[i] = '*';
    stars[i] = 0;

    snprintf(quality, sizeof(quality), "%s(%s)", prepend, stars);

    return (quality);
}

int main(int argc, char *argv[]) {

    if( argc <= 1 ) {
        printf("usage: %s image.jpeg\n", argv[0]);
        exit(1);
    }

    argc--;
    while(argc > 0) {
        argc--;
        argv++;
        char *name = argv[0];

        printf("detecting f5 in: %s\n", name);

        if (jpg_open(name) == -1) {
            printf("Couldn't open %s\n", name);
            continue;
        }

        double beta = detect_f5(name);
        char tmp[80];
        int stars;

        char outbuf[1024];
        sprintf(outbuf, "%s :", name);
        if (beta < 0.25) {
            // no f5
        } else {
            //f5 detected
            stars = 1;
            if (beta > 0.25)
                stars++;
            if (beta > 0.4)
                stars++;

            snprintf(tmp, sizeof(tmp), " f5[%f]", beta);
            strlcat(outbuf, quality(tmp, stars), sizeof(outbuf));
            printf("%s\n", outbuf);
        }
    }
}