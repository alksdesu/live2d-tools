#ifndef THEME_H
#define THEME_H

#include <QString>

namespace Theme {

inline QString globalStyleSheet()
{
    return QStringLiteral(R"(

/* 全局 */
* {
    font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
    font-size: 13px;
}

QMainWindow {
    background-color: #181B28;
}

QWidget {
    color: #C8CDD8;
    background-color: #181B28;
}

/* 工具栏 */
QToolBar {
    background-color: #1E2235;
    border: none;
    border-bottom: 1px solid #2A2F45;
    padding: 4px 8px;
    spacing: 6px;
}

QToolBar::separator {
    width: 1px;
    background-color: #2A2F45;
    margin: 4px 6px;
}

QToolBar QToolButton {
    background-color: transparent;
    color: #9BA3B8;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 5px 14px;
    font-size: 13px;
}

QToolBar QToolButton:hover {
    background-color: #262B42;
    color: #E0E4ED;
    border-color: #3A4060;
}

QToolBar QToolButton:pressed {
    background-color: #1A1E30;
}

/* 状态栏 */
QStatusBar {
    background-color: #151825;
    border-top: 1px solid #252940;
    color: #6B7394;
    font-size: 12px;
    padding: 0px;
}

QStatusBar::item {
    border: none;
}

QStatusBar QPushButton {
    background-color: transparent;
    color: #6B7394;
    border: 1px solid #2A2F45;
    border-radius: 10px;
    padding: 3px 12px;
    font-size: 11px;
    margin: 2px 3px;
}

QStatusBar QPushButton:hover {
    background-color: #222640;
    color: #A0A8C0;
    border-color: #3A4060;
}

QStatusBar QPushButton:checked {
    background-color: #1C3A5E;
    color: #6BAED6;
    border-color: #2E5A8A;
}

/* Dock 面板 */
QDockWidget {
    color: #E0E4ED;
    font-size: 13px;
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
}

QDockWidget::title {
    background-color: #1C2035;
    border-bottom: 2px solid #3B7DD8;
    padding: 7px 12px;
    text-align: left;
    font-weight: bold;
    font-size: 13px;
}

QDockWidget::close-button, QDockWidget::float-button {
    background: transparent;
    border: none;
    padding: 2px;
}

QDockWidget::close-button:hover, QDockWidget::float-button:hover {
    background-color: #2A2F45;
    border-radius: 3px;
}

/* 树控件 */
QTreeWidget, QTreeView {
    background-color: #1A1E30;
    border: none;
    outline: none;
    selection-background-color: transparent;
}

QTreeWidget::item, QTreeView::item {
    padding: 4px 6px;
    border-radius: 4px;
    margin: 1px 4px;
}

QTreeWidget::item:hover, QTreeView::item:hover {
    background-color: #222640;
}

QTreeWidget::item:selected, QTreeView::item:selected {
    background-color: #1C3254;
    color: #7DB8E8;
}

QTreeWidget::branch {
    background-color: transparent;
}

QHeaderView::section {
    background-color: #1E2235;
    color: #8B92A8;
    border: none;
    border-bottom: 1px solid #2A2F45;
    padding: 5px 8px;
    font-size: 12px;
}

/* 列表 */
QListWidget {
    background-color: #1A1E30;
    border: 1px solid #252940;
    border-radius: 6px;
    outline: none;
    padding: 2px;
}

QListWidget::item {
    padding: 5px 10px;
    border-radius: 4px;
    margin: 1px 2px;
}

QListWidget::item:hover {
    background-color: #222640;
}

QListWidget::item:selected {
    background-color: #1C3254;
    color: #7DB8E8;
}

/* 滚动条 */
QScrollArea {
    border: none;
    background-color: transparent;
}

QScrollBar:vertical {
    background-color: transparent;
    width: 8px;
    margin: 0;
}

QScrollBar::handle:vertical {
    background-color: #2E3350;
    border-radius: 4px;
    min-height: 30px;
}

QScrollBar::handle:vertical:hover {
    background-color: #3B4268;
}

QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: transparent;
    height: 0;
    border: none;
}

QScrollBar:horizontal {
    background-color: transparent;
    height: 8px;
    margin: 0;
}

QScrollBar::handle:horizontal {
    background-color: #2E3350;
    border-radius: 4px;
    min-width: 30px;
}

QScrollBar::handle:horizontal:hover {
    background-color: #3B4268;
}

QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
    background: transparent;
    width: 0;
    border: none;
}

/* 按钮 */
QPushButton {
    background-color: #232842;
    color: #B0B8D0;
    border: 1px solid #2E3350;
    border-radius: 6px;
    padding: 6px 16px;
    font-size: 13px;
}

QPushButton:hover {
    background-color: #2A3050;
    color: #E0E4ED;
    border-color: #3B4268;
}

QPushButton:pressed {
    background-color: #1A1E35;
}

QPushButton:disabled {
    background-color: #1A1E30;
    color: #454D68;
    border-color: #252940;
}

QPushButton#primaryBtn {
    background-color: #2B5EA8;
    color: #E8F0FF;
    border: none;
    font-weight: bold;
}

QPushButton#primaryBtn:hover {
    background-color: #3470C0;
}

QPushButton#primaryBtn:pressed {
    background-color: #224A88;
}

/* 滑条 */
QSlider::groove:horizontal {
    height: 4px;
    background-color: #252940;
    border-radius: 2px;
}

QSlider::handle:horizontal {
    width: 14px;
    height: 14px;
    margin: -5px 0;
    background-color: #4A90D9;
    border: 2px solid #1A1E30;
    border-radius: 8px;
}

QSlider::handle:horizontal:hover {
    background-color: #5BA0E8;
    border-color: #1A1E30;
}

QSlider::handle:horizontal:pressed {
    background-color: #3A78C0;
}

QSlider::sub-page:horizontal {
    background-color: #2B5EA8;
    border-radius: 2px;
}

/* 复选框 */
QCheckBox {
    spacing: 8px;
    color: #B0B8D0;
}

QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border: 2px solid #3A4060;
    border-radius: 4px;
    background-color: #1A1E30;
}

QCheckBox::indicator:hover {
    border-color: #4A90D9;
}

QCheckBox::indicator:checked {
    background-color: #2B5EA8;
    border-color: #2B5EA8;
    image: none;
}

QCheckBox::indicator:checked:hover {
    background-color: #3470C0;
    border-color: #3470C0;
}

/* 标签 */
QLabel {
    background-color: transparent;
    color: #9BA3B8;
}

/* 分割条 */
QSplitter::handle {
    background-color: #252940;
}

QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

/* 对话框 */
QDialog {
    background-color: #1C2035;
}

QDialogButtonBox QPushButton {
    min-width: 80px;
}

/* 进度条 */
QProgressBar {
    background-color: #1A1E30;
    border: 1px solid #252940;
    border-radius: 4px;
    text-align: center;
    color: #9BA3B8;
    height: 18px;
}

QProgressBar::chunk {
    background-color: #2B5EA8;
    border-radius: 3px;
}

/* 消息框 */
QMessageBox {
    background-color: #1C2035;
}

QMessageBox QLabel {
    color: #C8CDD8;
}

/* 提示 */
QToolTip {
    background-color: #252940;
    color: #D0D4E0;
    border: 1px solid #3A4060;
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 12px;
}

/* 标题栏 */
QToolBar#titleBar {
    background-color: #151825;
    border: none;
    border-bottom: 1px solid #1E2235;
    padding: 0 4px;
    spacing: 0;
}

QLabel#titleLabel {
    color: #6B7394;
    font-size: 12px;
    padding-left: 4px;
}

QPushButton#winBtnMin, QPushButton#winBtnMax, QPushButton#winBtnClose {
    background-color: transparent;
    border: none;
    border-radius: 0;
    color: #6B7394;
    font-size: 14px;
    padding: 0;
}

QPushButton#winBtnMin:hover, QPushButton#winBtnMax:hover {
    background-color: #252940;
    color: #C8CDD8;
}

QPushButton#winBtnClose:hover {
    background-color: #C42B1C;
    color: #FFFFFF;
}

/* 菜单 */
QMenu {
    background-color: #1E2235;
    border: 1px solid #2A2F45;
    border-radius: 6px;
    padding: 4px;
}

QMenu::item {
    padding: 6px 24px;
    border-radius: 4px;
}

QMenu::item:selected {
    background-color: #262B42;
    color: #E0E4ED;
}

QMenu::separator {
    height: 1px;
    background-color: #2A2F45;
    margin: 4px 8px;
}

    )");
}

} // namespace Theme

#endif // THEME_H
