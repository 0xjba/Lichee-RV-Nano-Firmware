#include <opencv2/opencv.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <stdio.h>
int main(int argc, char *argv[]) {
    const char *out_path = (argc > 1) ? argv[1] : "/root/capture.jpg";
    int warmup = (argc > 2) ? atoi(argv[2]) : 30;
    cv::VideoCapture cap;
    cap.open(0);
    cv::Mat bgr;
    for (int i = 0; i < warmup; i++) cap >> bgr;
    cap >> bgr;
    cv::imwrite(out_path, bgr);
    printf("Saved -> %s\n", out_path);
    cap.release();
    return 0;
}
