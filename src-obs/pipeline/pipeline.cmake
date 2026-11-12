# SPDX-License-Identifier: Apache-2.0

# src-obs/pipeline のファイルを cordmux.dll のターゲットに追加する。src-obs/CMakeLists.txt が
# include(pipeline/pipeline.cmake) で取り込む。libobs、FFmpeg、nlohmann-json、obs-frontend-api は、ほかのフォルダーの
# .cmake (config、decode、ws、sources) がこのターゲットにリンクしているので、ここでは find_package も
# target_link_libraries も呼ばない。コンパイルするのは pipeline.cpp だけで、ヘッダーは IDE のツリーに表示するためだけに
# 並べる。ヘッダーだけで実装したファイルは libobs に依存しないので、単体テスト (src-obs/tests/CMakeLists.txt) は
# インクルードするだけで使う。

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    pipeline/arrival_stats.h
    pipeline/arrival_window.h
    pipeline/audio_mixer.h
    pipeline/audio_meter.h
    pipeline/av_offset_plan.h
    pipeline/composite_audio_plan.h
    pipeline/facts_state.h
    pipeline/frame_delivery_plan.h
    pipeline/frame_pacer.h
    pipeline/hw_fallback_note.h
    pipeline/latency_stats.h
    pipeline/log_ring.h
    pipeline/mixer_roster.h
    pipeline/receive_restart.h
    pipeline/slot_release.h
    pipeline/stream_router.h
    pipeline/voice_activity.h
    pipeline/pipeline.h
    pipeline/pipeline.cpp
)
