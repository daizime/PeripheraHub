#include "app/widgets/DeviceCard.h"
#include "app/widgets/DeviceGrid.h"
#include <QApplication>
#include <QFont>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <array>
#include <iostream>
#include <vector>

using namespace peripheral::app;
int checks = 0;
int failures = 0;

void check(bool value, const char* message) {
    ++checks;
    if (!value) {
        ++failures;
        std::cout << "FAIL " << message << '\n';
    }
}

void settle() {
    for (int index = 0; index < 5; ++index)
        QApplication::processEvents();
}

void verify(DeviceGrid& grid, const std::array<DeviceCard*, 4>& cards, int mask, const QSize& size) {
    for (int index = 0; index < 4; ++index)
        grid.setPresent(cards[index], (mask & (1 << index)) != 0);
    grid.resize(size);
    settle();
    std::vector<DeviceCard*> visible;
    for (int index = 0; index < 4; ++index) {
        const bool present = (mask & (1 << index)) != 0;
        check(cards[index]->isVisible() == present, "only present cards visible");
        check(cards[index]->parentWidget() == &grid, "hidden cards still owned and retained");
        if (present)
            visible.push_back(cards[index]);
    }
    check(grid.visibleCardCount() == static_cast<int>(visible.size()), "present count");
    auto* empty = grid.findChild<QLabel*>("deviceGridEmpty");
    check(empty && empty->isVisible() == visible.empty(), "empty state visibility");
    for (std::size_t index = 0; index < visible.size(); ++index) {
        const auto geometry = visible[index]->geometry();
        check(grid.rect().contains(geometry), "card within available grid area");
        check(geometry.width() > 250 && geometry.height() > 230, "small window keeps useful card size");
        if (index == 0)
            check(geometry.topLeft() == QPoint(0, 0), "first present card occupies first slot");
        if (index == 1)
            check(geometry.y() == 0 && geometry.left() > visible[0]->geometry().right(),
                  "second present card occupies second slot");
        if (index == 2)
            check(geometry.x() == 0 && geometry.top() > visible[0]->geometry().bottom(),
                  "third present card occupies third slot");
        if (index == 3)
            check(geometry.top() == visible[2]->geometry().top() &&
                      geometry.left() > visible[2]->geometry().right(),
                  "fourth present card occupies fourth slot");
        if (visible.size() <= 2)
            check(geometry.height() <= 310, "single row leaves spare area below cards");
    }
    std::vector<QRect> before;
    for (auto* card : visible)
        before.push_back(card->geometry());
    for (int expansion = 0; expansion < 16; ++expansion) {
        for (int index = 0; index < 4; ++index)
            cards[index]->setExpanded((expansion & (1 << index)) != 0);
        settle();
        for (std::size_t index = 0; index < visible.size(); ++index)
            check(visible[index]->geometry() == before[index], "expansion keeps all visible card geometry");
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setFont(QFont("Microsoft YaHei UI", 10));
    DeviceGrid grid;
    grid.setStyleSheet(
        "QFrame#deviceCard { background:white; border:1px solid #dce3ef; border-radius:16px; }");
    grid.resize(956, 617);
    std::array<DeviceCard*, 4> cards;
    std::array<QPointer<DeviceCard>, 4> guards;
    for (int index = 0; index < 4; ++index) {
        cards[index] = new DeviceCard(QString("类别 %1").arg(index), QString("设备 %1").arg(index));
        cards[index]->setCardId(QString::number(index));
        cards[index]->quickLayout()->addWidget(new QLabel("常用控制"));
        cards[index]->quickLayout()->addStretch();
        for (int row = 0; row < 40; ++row)
            cards[index]->detailLayout()->addWidget(new QLabel(QString("详细功能 %1").arg(row)));
        guards[index] = cards[index];
        grid.addCard(cards[index]);
        check(cards[index]->isHidden(), "registration starts hidden");
    }
    grid.addCard(cards[0]);
    grid.setPresent(nullptr, true);
    check(grid.visibleCardCount() == 0, "duplicate registration and null do not create phantom cards");
    grid.show();
    settle();
    auto* empty = grid.findChild<QLabel*>("deviceGridEmpty");
    check(empty && empty->isVisible() && empty->text().contains("尚未检测到"), "default empty state");
    grid.setScanning(true);
    check(empty->text() == "正在识别设备…", "scanning text");
    grid.setScanning(false);
    check(empty->text().contains("接入后会自动显示"), "final empty text");
    for (const auto size : {QSize(956, 617), QSize(776, 547), QSize(956, 1200)})
        for (int mask = 0; mask < 16; ++mask)
            verify(grid, cards, mask, size);
    verify(grid, cards, 15, QSize(776, 547));
    cards[1]->setExpanded(true);
    grid.setPresent(cards[1], false);
    settle();
    check(!guards[1].isNull() && guards[1]->isExpanded(), "hide retains card and expanded state");
    grid.setPresent(cards[1], true);
    settle();
    check(cards[1]->isExpanded(), "reconnect retains expanded state");
    for (const auto& guard : guards)
        check(!guard.isNull(), "reflow never deletes registered cards");
    grid.grab().save("all-expanded.png");
    delete cards[2];
    settle();
    check(guards[2].isNull() && grid.visibleCardCount() == 3,
          "external card removal does not leave dangling entry");
    std::cout << "after external removal last card=" << cards[3]->geometry().x() << ','
              << cards[3]->geometry().y() << ' ' << cards[3]->geometry().width() << 'x'
              << cards[3]->geometry().height() << '\n';
    check(cards[3]->geometry().x() == 0 && cards[3]->geometry().y() > 0,
          "external removal compacts remaining card");
    std::cout << "CHECKS=" << checks << " FAILURES=" << failures
              << " (Qt-only fixture; no platform code or hardware API linked)\n";
    return failures == 0 ? 0 : 1;
}
