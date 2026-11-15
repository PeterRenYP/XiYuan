#pragma once
#include "transport.h"
#include <QMainWindow>

class QLineEdit;
class QLabel;
class QSpinBox;
class QTableWidget;
class QTextEdit;
class Heatmap;

class WeightFileWindow final : public QMainWindow {
    Q_OBJECT
  public:
    explicit WeightFileWindow(QWidget *parent=nullptr);
    ~WeightFileWindow() override;
    void openFile(const QString &path);
    QString submit(const QString &name,const QJsonObject &payload,bool external=false);
    QJsonObject state() const;
    bool busy() const {return !requests.isEmpty();}
    QString revision;
  signals:
    void received(const QJsonObject &message);
  private:
    void receive(const QJsonObject &message);
    void list();
    void slice();
    void preview();
    void renderList(const QJsonObject &payload);
    QString selectedName() const;
    Transport transport;
    QMap<QString,QPair<QString,bool>> requests;
    QString path,lastBackup;
    QLineEdit *search=nullptr;
    QLabel *notice=nullptr,*fileLabel=nullptr;
    QSpinBox *start=nullptr,*count=nullptr;
    QTableWidget *tensors=nullptr,*values=nullptr;
    QTextEdit *editValues=nullptr;
    Heatmap *sliceHeat=nullptr;
    QJsonObject previewRequest;
    int offset=0;
};
