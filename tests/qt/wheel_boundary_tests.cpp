#include "app/widgets/MonitorControls.h"
#include "app/widgets/UiTheme.h"
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <iostream>

using namespace peripheral::app;
int checks = 0, failures = 0;
void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cout << "FAIL " << description << '\n';
    }
}
void settle() {
    for (int index = 0; index < 6; ++index)
        QApplication::processEvents();
}
void wheel(QWidget* target, int delta, Qt::ScrollPhase phase = Qt::NoScrollPhase,
           bool emulateNativePropagation = false) {
    const auto global = target->mapToGlobal(QPointF(target->rect().center()));
    // Public sendEvent deliberately does not bubble synthetic wheels. For disabled inputs only,
    // reproduce QApplication's documented/local-source parent walk without private Qt APIs.
    for (auto* receiver = target; receiver; receiver = receiver->parentWidget()) {
        QWheelEvent event(receiver->mapFromGlobal(global), global, {}, QPoint(0, delta), Qt::NoButton,
                          Qt::NoModifier, phase, false);
        const bool handled = QApplication::sendEvent(receiver, &event);
        if (!emulateNativePropagation || (handled && event.isAccepted()) || receiver->isWindow())
            break;
    }
    settle();
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QScrollArea outer;
    outer.resize(420, 280);
    outer.setWidgetResizable(true);
    auto* content = new QWidget;
    auto* outerLayout = new QVBoxLayout(content);
    auto* inner = new QScrollArea;
    inner->setFixedHeight(190);
    inner->setWidgetResizable(true);
    auto* body = new QWidget;
    auto* innerLayout = new QVBoxLayout(body);
    auto* combo = createThemedComboBox();
    combo->addItems({"A", "B", "C"});
    combo->setCurrentIndex(1);
    auto* spin = createThemedSpinBox();
    spin->setRange(0, 100);
    spin->setValue(50);
    auto* slider = createThemedSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    slider->setValue(50);
    innerLayout->addWidget(combo);
    innerLayout->addWidget(spin);
    innerLayout->addWidget(slider);
    innerLayout->addSpacing(400);
    inner->setWidget(body);
    outerLayout->addWidget(inner);
    outerLayout->addSpacing(700);
    outer.setWidget(content);
    outer.show();
    settle();
    auto* innerBar = inner->verticalScrollBar();
    auto* outerBar = outer.verticalScrollBar();
    check(innerBar->maximum() > 0 && outerBar->maximum() > 0, "both nested scroll areas can scroll");
    int changed = 0;
    QObject::connect(combo, &QComboBox::currentIndexChanged, &outer, [&](int) { ++changed; });
    QObject::connect(spin, &QSpinBox::valueChanged, &outer, [&](int) { ++changed; });
    QObject::connect(slider, &QSlider::valueChanged, &outer, [&](int) { ++changed; });
    for (auto* input :
         {static_cast<QWidget*>(combo), static_cast<QWidget*>(spin),
          static_cast<QWidget*>(spin->findChild<QLineEdit*>()), static_cast<QWidget*>(slider)}) {
        innerBar->setValue(0);
        outerBar->setValue(0);
        wheel(input, -120);
        const int oneStep = innerBar->value();
        check(oneStep > 0 && outerBar->value() == 0, "nearest scroll area consumes exactly one wheel");
        innerBar->setValue(innerBar->maximum());
        wheel(input, -120);
        check(innerBar->value() == innerBar->maximum() && outerBar->value() > 0,
              "inner bottom boundary forwards wheel to outer scroll area");
        innerBar->setValue(0);
        outerBar->setValue(200);
        wheel(input, 120);
        check(innerBar->value() == 0 && outerBar->value() < 200,
              "inner top boundary forwards reverse wheel to outer scroll area");
        innerBar->setValue(0);
        outerBar->setValue(0);
        wheel(input, 0, Qt::ScrollBegin);
        wheel(input, -120, Qt::ScrollUpdate);
        wheel(input, 0, Qt::ScrollEnd);
        check(innerBar->value() == oneStep && outerBar->value() == 0,
              "phase begin update end never duplicates scroll movement");
        input->setEnabled(false);
        innerBar->setValue(0);
        outerBar->setValue(0);
        wheel(input, -120, Qt::NoScrollPhase, true);
        check(innerBar->value() > 0 && outerBar->value() == 0 && changed == 0,
              "disabled input follows native parent propagation without modifying a value");
        input->setEnabled(true);
    }
    check(changed == 0 && combo->currentIndex() == 1 && spin->value() == 50 && slider->value() == 50,
          "all nested boundary input values remain unchanged");

    MonitorControls controls;
    peripheral::platform::MonitorControlsInfo info;
    info.id = L"simulated-target-A";
    controls.setInfo(info);
    auto* status = controls.findChild<QLabel*>("monitorControlsStatus");
    controls.setOperationStatus("simulated old final", true);
    controls.setBusy(true);
    controls.setInfo(info);
    check(status->text().contains("simulated old final"), "same-target background data keeps final feedback");
    controls.setOperationStatus("simulated new progress");
    controls.setOperationStatus("simulated new failure", true);
    controls.setBusy(false);
    controls.setInfo(info);
    check(status->text() == "simulated new failure", "new failed operation replaces old final feedback");
    info.id = L"simulated-target-B";
    controls.setInfo(info);
    check(!status->text().contains("simulated new failure"),
          "target change clears failed operation feedback");
    controls.setOperationStatus("simulated detach while busy", true);
    controls.setBusy(true);
    controls.clear();
    check(!status->text().contains("simulated detach"), "clear target discards old feedback");
    check(controls.findChild<QPushButton*>("monitorControlsRefresh")->isEnabled(),
          "clear target also resets former busy state");

    QScrollArea focusArea;
    focusArea.resize(440, 230);
    focusArea.setWidgetResizable(true);
    auto* focusContent = new QWidget;
    auto* focusLayout = new QVBoxLayout(focusContent);
    auto* focusControls = new MonitorControls;
    peripheral::platform::MonitorFeatureInfo contrast;
    contrast.code = 0x12;
    contrast.advertised = contrast.readable = contrast.writable = true;
    contrast.current = 50;
    contrast.maximum = 100;
    info.features = {contrast};
    focusControls->setInfo(info);
    focusLayout->addWidget(focusControls);
    auto* unrelatedEditor = createThemedSpinBox();
    focusLayout->addWidget(unrelatedEditor);
    focusArea.setWidget(focusContent);
    focusArea.show();
    focusArea.activateWindow();
    settle();
    auto* apply = focusControls->findChild<QPushButton*>("monitorFeature18Apply");
    apply->setFocus(Qt::OtherFocusReason);
    settle();
    check(apply->hasFocus(), "native apply button initially owns keyboard focus");
    const int beforeBusy = focusArea.verticalScrollBar()->value();
    focusControls->setBusy(true);
    settle();
    check(!unrelatedEditor->hasFocus(), "disabling apply never transfers focus to later brightness-like input");
    check(focusArea.verticalScrollBar()->value() == beforeBusy,
          "entering native busy state preserves card scroll position");
    std::cout << "CHECKS=" << checks << " FAILURES=" << failures << " NO_DEVICE_BACKEND_LINKED\n";
    return failures ? 1 : 0;
}
