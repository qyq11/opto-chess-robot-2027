#ifndef OPTO_MOTOR_CONTROL_H
#define OPTO_MOTOR_CONTROL_H

#include <glib.h>

typedef struct MotorControl MotorControl;

/* 不传串口时只提供禁用状态；串口由本对象独占，计时不依赖网页或相机。 */
MotorControl *motor_control_new(const char *port);
void motor_control_free(MotorControl *motor);
char *motor_control_json(MotorControl *motor);
/* 只接受查询、前后左右定时移动、前进指定距离、定时动作调速和停止；不接受任意串口文本。 */
char *motor_control_command(MotorControl *motor, GHashTable *fields, unsigned *status);

#endif
