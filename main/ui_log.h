#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// logs to serial always; also pushes to on-screen log widget if ui_log_bind_label was called
void ui_log(const char *fmt, ...);

// called by ui.cpp once the log label widget exists
void ui_log_bind_label(void *lv_label_obj);

#ifdef __cplusplus
}
#endif