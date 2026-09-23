#ifndef GAIME_CALIBRATION_HID_H
#define GAIME_CALIBRATION_HID_H
#include <stddef.h>
#include <stdint.h>

enum { CALIB_HID_OK = 0, CALIB_HID_DEVICE = -1, CALIB_HID_WRITE = -2,
       CALIB_HID_NO_RESPONSE = -3, CALIB_HID_REJECTED = -4 };

uint16_t calib_crc16(const unsigned char *data, size_t length);
void calib_build_report(unsigned char report[65], int point,
                        const int actual_xy[6], int target_x, int target_y);
void calib_build_profile_report(unsigned char report[65], int profile);
void calib_build_status_report(unsigned char report[65]);
int calib_parse_status(const unsigned char *report, size_t length,
                       int *active_profile, unsigned *valid_mask,
                       int *last_result, int *last_profile);
int calib_parse_response(const unsigned char *report, size_t length, int *final_result);
int calib_select_profile(const char *physical, const char *unique, int profile,
                         char *device_path, size_t path_size);
int calib_transmit(const char *physical, const char *unique,
                   int profile, const int coords[48], const int targets[16],
                   char *device_path, size_t path_size);
#endif
