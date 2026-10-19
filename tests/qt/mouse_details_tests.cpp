#include "app/widgets/MouseDetails.h"
#include "../fixtures/vgn_f2_capture.h"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <iostream>
using namespace peripheral;
int main(int argc, char** argv) {
    QApplication app(argc,argv);
    app::MouseDetails view;
    int failures = 0;
    const auto check = [&](bool ok,const char* text) { if (!ok) { ++failures; std::cerr << text << '\n'; } };
    check(view.isHidden(), "unrelated mouse has no private details");
    const auto profile = fixture::f2Capture().decode();
    if (!profile) return 1;
    view.setInfo(&*profile, QDateTime::currentMSecsSinceEpoch(), false);
    const auto stages = view.findChild<QLabel*>("mouseDpiStages");
    const auto keys = view.findChild<QLabel*>("mouseButtonMappings");
    check(!view.isHidden() && stages && stages->text().contains("42000 DPI"), "extension bits reach UI");
    check(stages && stages->text().contains("3200 DPI · 当前档位"), "active stage distinguished");
    check(keys && keys->text().contains("DPI 循环"), "captured button label");
    view.setInfo(&*profile, QDateTime::currentMSecsSinceEpoch(), true, "timeout");
    bool stale = false;
    for (const auto label : view.findChildren<QLabel*>()) stale |= label->text().contains("上次读数");
    check(stale, "failed attempt does not make previous configuration fresh");

    // 写入区：默认不出现，开启后按当前读数预填，点击只发出意图。
    auto* editor = view.findChild<QWidget*>("mouseWriteEditor");
    auto* dpiInput = view.findChild<QSpinBox*>();
    auto* dpiApply = view.findChild<QPushButton*>("mouseDpiApply");
    auto* rateApply = view.findChild<QPushButton*>("mouseRateApply");
    auto* stageApply = view.findChild<QPushButton*>("mouseStageApply");
    auto* rateInput = view.findChild<QComboBox*>("mouseRateInput");
    auto* stageInput = view.findChild<QComboBox*>("mouseStageInput");
    check(editor && dpiApply && rateApply && stageApply && rateInput && stageInput,
          "write controls exist but stay hidden by default");
    check(editor && editor->isHidden(), "write editor is hidden until explicitly enabled");
    const auto hasLabel = [&](const char* phrase) {
        for (const auto label : view.findChildren<QLabel*>())
            if (label->text().contains(QString::fromUtf8(phrase)))
                return true;
        return false;
    };
    check(hasLabel("配置目前只读"), "closed write switch is stated as read-only");
    view.setWritable(true);
    check(hasLabel("写入已开启"), "enabling writes updates the state line instead of still claiming read-only");
    check(editor && !editor->isHidden(), "enabling writes reveals the editor for a known profile");
    check(dpiInput && dpiInput->value() == 3200, "editor starts from the active stage DPI");
    check(rateInput && rateInput->currentData().toInt() == 1000, "editor starts from the current polling rate");
    check(stageInput && stageInput->currentData().toInt() == profile->activeStage,
          "editor starts from the active stage index");
    int dpiRequest = 0, rateRequest = 0, stageRequest = -1;
    QObject::connect(&view, &app::MouseDetails::dpiWriteRequested, [&](int value) { dpiRequest = value; });
    QObject::connect(&view, &app::MouseDetails::rateWriteRequested, [&](int value) { rateRequest = value; });
    QObject::connect(&view, &app::MouseDetails::stageWriteRequested, [&](int value) { stageRequest = value; });
    dpiInput->setValue(1600);
    dpiApply->click();
    rateInput->setCurrentIndex(rateInput->findData(500));
    rateApply->click();
    stageInput->setCurrentIndex(1);
    stageApply->click();
    check(dpiRequest == 1600 && rateRequest == 500 && stageRequest == 1,
          "each apply button emits exactly the user's intent");
    view.setWritable(false);
    check(editor && editor->isHidden(), "turning the switch off hides the editor again");

    // 用户改过的数值不被后台刷新覆盖；没改过时继续跟随设备。
    view.setWritable(true);
    dpiInput->setValue(2800);
    auto shifted = *profile;
    shifted.stages[shifted.activeStage] = {1600, 1600};
    view.setInfo(&shifted, QDateTime::currentMSecsSinceEpoch(), false);
    check(dpiInput->value() == 2800, "a background refresh leaves a user-edited DPI value untouched");
    view.setInfo(&shifted, QDateTime::currentMSecsSinceEpoch(), false);
    check(dpiInput->value() == 2800, "repeated refreshes do not accumulate the user's edit");
    dpiInput->setValue(1600);
    view.setInfo(&*profile, QDateTime::currentMSecsSinceEpoch(), false);
    check(dpiInput->value() == 3200, "an unedited editor follows the device reading again");

    view.setInfo(nullptr);
    check(view.isHidden() && stages->text().isEmpty() && keys->text().isEmpty(), "selection clears previous mouse");
    check(app::MouseDetails::dpiText({400,800}).contains("X 400 / Y 800"), "independent axes visible");
    return failures ? 1 : 0;
}
