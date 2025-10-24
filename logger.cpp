#include "logger.hpp"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutexLocker>
#include <QTextStream>
#include <iostream>

logger *logger::_inst = nullptr;

logger::logger()
{
  QMutexLocker locker(&m);
  if (QString(BRANCH) == "main")
    logFolderName = "C:/ProgramData/myrsm/";
  else
    logFolderName = QString() + "C:/ProgramData/myrsm_" + BRANCH + "/";
  max_logs_files = 7;
  if (!QDir(logFolderName).exists())
    QDir().mkdir(logFolderName);

  deleteOldLogs();   // delete old log files
  initLogFileName(); // create the logfile name

  if (_outFile.isOpen())
    qInstallMessageHandler(logger::handler);
  std_err_log = false;
  if (qgetenv("STDERR_LOG") == "1")
    std_err_log = true;
}

logger::~logger() {}

void logger::init()
{
  if (!_inst)
    _inst = new logger();
}

void logger::uninit()
{
  if (_inst)
  {
    delete _inst;
    _inst = nullptr;
  }
}

void logger::initLogFileName()
{
  QDateTime _logDateTime = QDateTime::currentDateTimeUtc();

  logFileName = QString(logFolderName + "/%1.txt")
                    .arg(_logDateTime.toString("yyyy_MM_dd_hh_mm_ss_zzz"));
  logDate = _logDateTime.date();
  if (_outFile.isOpen())
    _outFile.close();
  _outFile.setFileName(logFileName);
  if (_outFile.open(QIODevice::WriteOnly | QIODevice::Append) == false)
  {
    if (std_err_log)
      std::cerr << "Cannot open output file:" << logFileName.toLatin1().data();
    else
      std::cout << "Cannot open output file:" << logFileName.toLatin1().data();
  }
}

/**
 * @brief deletes old log files, only the last ones are kept
 */
void logger::deleteOldLogs()
{
  QDir dir;
  dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
  dir.setSorting(QDir::Time | QDir::Reversed);
  dir.setPath(logFolderName);

  QFileInfoList list = dir.entryInfoList();
  if (list.size() <= max_logs_files)
  {
    return; // no files to delete
  }
  else
  {
    for (int i = 0; i < (list.size() - max_logs_files); i++)
    {
      QString path = list.at(i).absoluteFilePath();
      QFile file(path);
      file.remove();
    }
  }
}

QString tag(QtMsgType type)
{
  switch (type)
  {
  case QtDebugMsg:
    return "debug";
  case QtWarningMsg:
    return "warning";
  case QtCriticalMsg:
    return "critical";
  case QtFatalMsg:
    return "fatal";
  case QtInfoMsg:
    return "info";
  default:
    return "??";
  }
}

void logger::_handler(QtMsgType type, const QMessageLogContext &context,
                      const QString &txt)
{
  QMutexLocker locker(&m);
  QString tStream;
  QTextStream ts(&tStream);
  QByteArray buffer;
  qint64 writen;
  qint64 pos;

  if (logDate != QDateTime::currentDateTimeUtc().date())
  {
    deleteOldLogs();
    initLogFileName();
  }
  ts << tag(type) << ":"
     << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << ":("
     << context.file << ":" << context.line << "):" << context.function << ":"
     << txt << Qt::endl;
  if (std_err_log)
    std::cerr << tStream.toStdString();
  buffer = tStream.toLatin1();
  if (_outFile.isOpen())
  {
    pos = 0;
    while (pos < buffer.size())
    {
      writen = _outFile.write(&(buffer.constData()[pos]));
      if (writen > 0)
        pos += writen;
      else
      {
        std::cerr << "Could not write log to "
                  << _outFile.fileName().toStdString() << std::endl;
        _outFile.close();
        logDate = QDate();
        return;
      }
    }
    _outFile.flush();
  }
}

void logger::handler(QtMsgType type, const QMessageLogContext &context,
                     const QString &txt)
{
  if (_inst)
  {
    _inst->_handler(type, context, txt);
  }
}
