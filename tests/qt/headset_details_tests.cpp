#include "app/widgets/HeadsetDetails.h"
#include <QApplication>
#include <QLabel>
#include <iostream>
using namespace peripheral;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app::HeadsetDetails view;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) { if (!ok) { ++failures; std::cerr << message << '\n'; } };
    check(view.isHidden(), "unknown headset has no private-control panel");
    platform::N9ControlsInfo info;
    info.status = n9::Status{};
    info.peq[0] = n9::PeqPoint{64,-20,9,1,false};
    view.setInfo(&info);
    check(!view.isHidden(), "matched headset exposes its independent details");
    auto* text = view.findChild<QLabel*>("headsetPeqReadings");
    check(text && text->text().contains("64 Hz") && text->text().contains("-2.0 dB") && text->text().contains("Q 0.9"),
          "frequency, signed gain and Q retain their device units");
    info.status.reset();
    view.setInfo(&info);
    check(text && text->text().isEmpty(), "failed reads cannot display previous headset EQ");
    view.setInfo(nullptr);
    check(view.isHidden(), "switching to unrelated audio hides private controls");
    return failures == 0 ? 0 : 1;
}
