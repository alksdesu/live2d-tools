#ifndef THEME_H
#define THEME_H

#include <QMap>
#include <QString>

namespace Theme {

inline QMap<QString, QString> tokens(bool dark = false)
{
    QMap<QString, QString> token = {
        {QStringLiteral("@window@"), QStringLiteral("#F5F7FA")},
        {QStringLiteral("@surface@"), QStringLiteral("#FFFFFF")},
        {QStringLiteral("@surfaceMuted@"), QStringLiteral("#F8F9FB")},
        {QStringLiteral("@surfaceHover@"), QStringLiteral("#F0F3F7")},
        {QStringLiteral("@border@"), QStringLiteral("#E2E6EC")},
        {QStringLiteral("@borderStrong@"), QStringLiteral("#D3D9E2")},
        {QStringLiteral("@text@"), QStringLiteral("#20242B")},
        {QStringLiteral("@textMuted@"), QStringLiteral("#606B7A")},
        {QStringLiteral("@textSubtle@"), QStringLiteral("#657182")},
        {QStringLiteral("@accent@"), QStringLiteral("#1677FF")},
        {QStringLiteral("@accentFill@"), QStringLiteral("#0864D8")},
        {QStringLiteral("@accentInk@"), QStringLiteral("#075DC7")},
        {QStringLiteral("@accentHover@"), QStringLiteral("#0754B8")},
        {QStringLiteral("@accentSoft@"), QStringLiteral("#E8F1FF")},
        {QStringLiteral("@danger@"), QStringLiteral("#C72D22")},
        {QStringLiteral("@dangerSoft@"), QStringLiteral("#FDE8E7")},
        {QStringLiteral("@accentText@"), QStringLiteral("#FFFFFF")},
        {QStringLiteral("@focus@"), QStringLiteral("#0864D8")},
        {QStringLiteral("@focusWidth@"), QStringLiteral("2px")},
        {QStringLiteral("@focusInset@"), QStringLiteral("-2px")},
        {QStringLiteral("@radius@"), QStringLiteral("8px")},
        {QStringLiteral("@radiusSmall@"), QStringLiteral("6px")},
        {QStringLiteral("@itemCheckSize@"), QStringLiteral("14px")},
        {QStringLiteral("@itemCheckRadius@"), QStringLiteral("3px")},
        {QStringLiteral("@borderWidth@"), QStringLiteral("1px")},
        {QStringLiteral("@space@"), QStringLiteral("8px")},
    };
    if (dark) {
        token.insert(QStringLiteral("@window@"), QStringLiteral("#16181D"));
        token.insert(QStringLiteral("@surface@"), QStringLiteral("#20242B"));
        token.insert(QStringLiteral("@surfaceMuted@"), QStringLiteral("#1B1E24"));
        token.insert(QStringLiteral("@surfaceHover@"), QStringLiteral("#2A3039"));
        token.insert(QStringLiteral("@border@"), QStringLiteral("#353C48"));
        token.insert(QStringLiteral("@borderStrong@"), QStringLiteral("#46505F"));
        token.insert(QStringLiteral("@text@"), QStringLiteral("#F4F6F8"));
        token.insert(QStringLiteral("@textMuted@"), QStringLiteral("#B7C0CC"));
        token.insert(QStringLiteral("@textSubtle@"), QStringLiteral("#8994A3"));
        token.insert(QStringLiteral("@accentSoft@"), QStringLiteral("#173B70"));
        token.insert(QStringLiteral("@focus@"), QStringLiteral("#8DBBFF"));
        token.insert(QStringLiteral("@accentInk@"), QStringLiteral("#A2C8FF"));
        token.insert(QStringLiteral("@danger@"), QStringLiteral("#FFB4AC"));
        token.insert(QStringLiteral("@dangerSoft@"), QStringLiteral("#52211F"));
    }

    return token;
}

inline QString globalStyleSheet(bool dark = false)
{
    const auto token = tokens(dark);
    QString style = QStringLiteral(R"(
* {
    font-family: "SF Pro Display", "Microsoft YaHei UI", "Segoe UI", sans-serif;
    font-size: 13px;
    color: @text@;
}

QMainWindow, QWidget {
    background: @window@;
}

CXYParameterPad, CPhysicsGraphWidget, CMotionCurveView {
    background: @surface@;
    selection-background-color: @accentInk@;
    selection-color: @surface@;
}

QToolBar {
    background: @surface@;
    border: none;
    border-bottom: 1px solid @border@;
    padding: 6px 10px;
    spacing: 6px;
}

QToolBar#titleBar {
    background: @surface@;
    border-bottom: 1px solid @border@;
    padding: 0 8px;
    spacing: 2px;
}

QToolBar QToolButton {
    background: transparent;
    color: @textMuted@;
    border: 1px solid transparent;
    border-radius: @radiusSmall@;
    padding: 7px 12px;
}

QToolBar QToolButton:hover {
    background: @surfaceHover@;
    color: @text@;
    border-color: @border@;
}

QToolBar QToolButton:pressed {
    background: @accentSoft@;
    color: @accentInk@;
}

QToolBar QToolButton:checked {
    background: @accentSoft@;
    color: @accentInk@;
    border-color: @accent@;
    font-weight: 600;
}

QPushButton {
    background: @surface@;
    color: @text@;
    border: 1px solid @borderStrong@;
    border-radius: @radiusSmall@;
    padding: 7px 14px;
}

QPushButton:hover {
    background: @surfaceHover@;
    border-color: @focus@;
}

QPushButton:pressed {
    background: @accentSoft@;
    color: @accentInk@;
}

QPushButton:disabled {
    background: @surfaceMuted@;
    color: @textSubtle@;
    border-color: @border@;
}

QPushButton#primaryBtn {
    background: @accentFill@;
    color: @accentText@;
    border-color: @accent@;
    font-weight: 600;
}

QPushButton#primaryBtn:hover {
    background: @accentHover@;
    border-color: @accentHover@;
}

QPushButton#dangerBtn {
    color: @danger@;
}

QPushButton#primaryBtn:disabled, QPushButton#dangerBtn:disabled {
    background: @surfaceMuted@;
    color: @textSubtle@;
    border-color: @border@;
}

QLabel {
    background: transparent;
    color: @textMuted@;
}

QLabel#titleLabel {
    color: @text@;
    font-size: 13px;
    font-weight: 600;
    padding-left: 6px;
}

QFrame#emptyCard {
    background: @surface@;
    border: 1px solid @border@;
    border-radius: 16px;
}

QLabel#emptyKicker {
    color: @accentInk@;
    font-size: 12px;
    font-weight: 600;
}

QLabel#emptyTitle {
    color: @text@;
    font-size: 26px;
    font-weight: 600;
}

QLabel#emptyHint {
    color: @textMuted@;
    line-height: 1.4em;
}

QFrame#canvasToolbar {
    background: @surface@;
    border-bottom: 1px solid @border@;
}

QLabel#canvasTitle {
    color: @text@;
    font-weight: 600;
}

QLabel#canvasMeta, QLabel#inspectorEmpty {
    color: @textSubtle@;
}

QLabel#physicsFpsLabel {
    color: @text@;
    font-weight: 600;
    margin-bottom: 4px;
}

QLabel#inspectorEmpty {
    padding: 8px;
}

QLabel#recentTitle {
    color: @textMuted@;
    font-weight: 600;
    padding-top: 4px;
}

QLabel#texInfoLabel {
    color: @textSubtle@;
    padding: 2px 4px;
}

QLabel#blendParamTag {
    background: @accentSoft@;
    color: @accentInk@;
    padding: 2px;
    border-radius: @radiusSmall@;
}

QPushButton#recentModel {
    text-align: left;
    color: @textMuted@;
    background: @surfaceMuted@;
    border-color: @border@;
}

QPushButton#recentModel:hover {
    color: @text@;
    background: @surfaceHover@;
    border-color: @focus@;
}

QPushButton#winBtnMin, QPushButton#winBtnMax, QPushButton#winBtnClose {
    background: transparent;
    border: none;
    border-radius: @radiusSmall@;
    color: @textMuted@;
    font-size: 15px;
    padding: 0;
}

QPushButton#winBtnMin:hover, QPushButton#winBtnMax:hover {
    background: @surfaceHover@;
    color: @text@;
}

QPushButton#winBtnClose:hover {
    background: @dangerSoft@;
    color: @danger@;
}

QDockWidget {
    color: @text@;
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
}

QDockWidget::title {
    background: @surface@;
    border: none;
    border-bottom: 1px solid @border@;
    padding: 10px 12px;
    text-align: left;
    font-weight: 600;
}

QDockWidget::close-button, QDockWidget::float-button {
    background: transparent;
    border: none;
    padding: 2px;
}

QTabWidget::pane {
    background: @surface@;
    border: 1px solid @border@;
    border-radius: @radius@;
    top: -1px;
}

QTabBar::tab {
    background: transparent;
    color: @textMuted@;
    border: @focusWidth@ solid transparent;
    padding: 8px 12px;
    margin-right: 2px;
}

QTabBar::tab:hover {
    color: @text@;
    background: @surfaceHover@;
    border-radius: @radiusSmall@;
}

QTabBar::tab:selected {
    color: @accentInk@;
    background: @accentSoft@;
    border-radius: @radiusSmall@;
    font-weight: 600;
}

QTreeWidget, QTreeView, CModelHierarchyView, QListWidget, QTableWidget {
    background: @surface@;
    alternate-background-color: @surfaceMuted@;
    border: 1px solid @border@;
    border-radius: @radius@;
    outline: none;
    selection-background-color: @accentSoft@;
    selection-color: @accentInk@;
}

QTreeWidget::item, QTreeView::item, CModelHierarchyView::item, QListWidget::item {
    padding: 6px 8px;
    border-radius: @radiusSmall@;
    margin: 1px 4px;
}

QTreeWidget::item:hover, QTreeView::item:hover, CModelHierarchyView::item:hover, QListWidget::item:hover {
    background: @surfaceHover@;
}

QTreeWidget::item:selected, QTreeView::item:selected, CModelHierarchyView::item:selected, QListWidget::item:selected {
    background: @accentSoft@;
    color: @accentInk@;
}

QTreeWidget::indicator, CModelHierarchyView::indicator, QTableWidget::indicator, QListWidget::indicator {
    width: @itemCheckSize@;
    height: @itemCheckSize@;
}

QTreeWidget::indicator:checked, CModelHierarchyView::indicator:checked, QTableWidget::indicator:checked, QListWidget::indicator:checked {
    background: @accent@;
    border: @borderWidth@ solid @accentHover@;
    border-radius: @itemCheckRadius@;
}

QTreeWidget::indicator:unchecked, CModelHierarchyView::indicator:unchecked, QTableWidget::indicator:unchecked, QListWidget::indicator:unchecked {
    background: @surface@;
    border: @borderWidth@ solid @borderStrong@;
    border-radius: @itemCheckRadius@;
}

QHeaderView::section {
    background: @surfaceMuted@;
    color: @textMuted@;
    border: none;
    border-bottom: 1px solid @border@;
    padding: 7px 8px;
    font-weight: 600;
}

QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: @surface@;
    color: @text@;
    border: 1px solid @borderStrong@;
    border-radius: @radiusSmall@;
    padding: 6px 8px;
    min-height: 18px;
    placeholder-text-color: @textSubtle@;
    selection-background-color: @accentSoft@;
    selection-color: @accentInk@;
}

QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
    border-color: @accent@;
}

QComboBox QAbstractItemView {
    background: @surface@;
    border: 1px solid @border@;
    selection-background-color: @accentSoft@;
    selection-color: @accentInk@;
}

QCheckBox, QRadioButton {
    spacing: 8px;
    color: @textMuted@;
    border: @focusWidth@ solid transparent;
    border-radius: @radiusSmall@;
}

QCheckBox::indicator, QRadioButton::indicator {
    width: 16px;
    height: 16px;
    border: 1px solid @borderStrong@;
    border-radius: @radiusSmall@;
    background: @surface@;
}

QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background: @accent@;
    border-color: @accent@;
}

QSlider {
    border: @focusWidth@ solid transparent;
    border-radius: @radiusSmall@;
}

QSlider::groove:horizontal {
    height: 4px;
    background: @border@;
    border-radius: 2px;
}

QSlider::sub-page:horizontal {
    background: @accent@;
    border-radius: 2px;
}

QSlider::handle:horizontal {
    width: 14px;
    height: 14px;
    margin: -5px 0;
    background: @surface@;
    border: 2px solid @accent@;
    border-radius: 8px;
}

QScrollBar:vertical, QScrollBar:horizontal {
    background: transparent;
}

QScrollBar:vertical { width: 8px; margin: 2px; }
QScrollBar:horizontal { height: 8px; margin: 2px; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
    background: @borderStrong@;
    border-radius: 4px;
    min-height: 28px;
    min-width: 28px;
}
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background: @textSubtle@; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: transparent; border: none; }

QGroupBox {
    background: @surface@;
    border: 1px solid @border@;
    border-radius: @radius@;
    margin-top: 12px;
    padding: 12px 8px 8px;
    font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: @text@;
}

QSplitter::handle { background: @border@; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

QStatusBar {
    background: @surface@;
    border-top: 1px solid @border@;
    color: @textMuted@;
}
QStatusBar::item { border: none; }
QStatusBar QPushButton {
    background: transparent;
    color: @textMuted@;
    border: none;
    border-radius: @radiusSmall@;
    padding: 5px 10px;
    margin: 1px;
}
QStatusBar QPushButton:hover { background: @surfaceHover@; color: @text@; }
QStatusBar QPushButton:checked { background: @accentSoft@; color: @accentInk@; }

QProgressBar {
    background: @surfaceMuted@;
    border: 1px solid @border@;
    border-radius: @radiusSmall@;
    text-align: center;
    color: @textMuted@;
    height: 18px;
}
QProgressBar::chunk { background: @accent@; border-radius: 4px; }

QDialog, QMessageBox {
    background: @surface@;
}
QToolTip {
    background: @text@;
    color: @window@;
    border: none;
    border-radius: @radiusSmall@;
    padding: 6px 8px;
}
QPushButton:focus, QToolButton:focus,
QTreeWidget:focus, QTreeView:focus, CModelHierarchyView:focus, QListWidget:focus, QTableWidget:focus {
    outline: @focusWidth@ solid @focus@;
    outline-offset: @focusInset@;
    outline-radius: @radiusSmall@;
}
QPushButton#primaryBtn:focus { outline-color: @accentText@; }
QTableWidget:focus { border-color: @focus@; }
QTabBar::tab:focus, QCheckBox:focus, QRadioButton:focus, QSlider:focus { border-color: @focus@; }
)" );
    for (auto it = token.cbegin(); it != token.cend(); ++it) style.replace(it.key(), it.value());
    return style;
}

}

#endif
