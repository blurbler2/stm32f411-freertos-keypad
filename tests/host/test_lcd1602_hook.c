/*
 * Replaces the weak lcd1602_ready_changed() of the driver, like the application
 * does in app.c. In a separate file because test_lcd1602.c includes the driver
 * source, and a weak and a normal definition cannot be in the same file.
 */
#include <stdbool.h>

int  hook_calls;
bool hook_last;

void lcd1602_ready_changed(bool ready);

void lcd1602_ready_changed(bool ready)
{
    hook_calls++;
    hook_last = ready;
}
