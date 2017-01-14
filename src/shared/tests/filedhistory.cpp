#include "../server/filedhistory.h"
#include "../util/passwordhash.h"
#include "../net/meta.h"

#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <memory>

using namespace server;

class TestFiledHistory: public QObject
{
	Q_OBJECT
private slots:
	void initTestCase()
	{
		QVERIFY(m_tempdir.isValid());
		m_dir = m_tempdir.path();
	}

	// Test that all metadata is stored correctly
	void testMetadata()
	{
		const QString password = "pass";
		const int maxUsers = 11;
		const QString title = "Hello world";
		const QString idAlias = "test";
		const protocol::ProtocolVersion protover = protocol::ProtocolVersion::current();
		const QString founder = "me!";
		const SessionHistory::Flags flags = SessionHistory::Persistent | SessionHistory::PreserveChat | SessionHistory::Nsfm;

		const QString bannedUser = "troll user with a long \\ \"name}%[";
		const QString opUser = "op";
		const QHostAddress bannedAddress("::ffff:192.168.0.100");

		const QString announcementUrl = "http://example.com/";

		QUuid testId = QUuid::createUuid();
		{
			std::unique_ptr<FiledHistory> fh { FiledHistory::startNew(m_dir, testId, idAlias, protover, founder) };
			QVERIFY(fh.get());
			fh->setPassword(password);
			fh->setMaxUsers(200);
			fh->setMaxUsers(maxUsers); // this should replace the previously set value
			fh->setTitle(title);
			fh->setFlags(flags);
			fh->addBan(bannedUser, bannedAddress, opUser);
			fh->addBan("test", QHostAddress("192.168.0.101"), opUser);
			fh->removeBan(2);
			fh->addAnnouncement(announcementUrl);
			fh->addAnnouncement("http://example.com/2/");
			fh->removeAnnouncement("http://example.com/2/");
		}

		{
			std::unique_ptr<FiledHistory> fh { FiledHistory::load(m_dir.absoluteFilePath(FiledHistory::journalFilename(testId))) };
			QVERIFY(fh.get());

			QCOMPARE(fh->id(), testId);
			QCOMPARE(fh->idAlias(), idAlias);
			QCOMPARE(fh->founderName(), founder);
			QCOMPARE(fh->protocolVersion(), protover);
			QVERIFY(passwordhash::check(password, fh->passwordHash()));
			QCOMPARE(fh->maxUsers(), maxUsers);
			QCOMPARE(fh->title(), title);
			QCOMPARE(fh->flags(), flags);

			QJsonArray banlist = fh->banlist().toJson(true);
			QCOMPARE(banlist.size(), 1);
			QCOMPARE(banlist.at(0).toObject()["username"].toString(), bannedUser);
			QCOMPARE(banlist.at(0).toObject()["bannedBy"].toString(), opUser);
			QCOMPARE(banlist.at(0).toObject()["ip"].toString(), bannedAddress.toString());

			QStringList announcements = fh->announcements();
			QCOMPARE(announcements.size(), 1);
			QCOMPARE(announcements.at(0), announcementUrl);
		}
	}

	// Test that a recording can be loaded correctly
	void testLoading()
	{
		QString file = makeTestRecording();
		std::unique_ptr<FiledHistory> fh { FiledHistory::load(m_dir.absoluteFilePath(file)) };

		QList<protocol::MessagePtr> msgs;
		int lastIdx;

		std::tie(msgs, lastIdx) = fh->getBatch(-1);

		QCOMPARE(msgs.size(), 3);
		QCOMPARE(msgs.at(0).cast<protocol::Chat>().message(), QString("test1"));
		QCOMPARE(lastIdx, 2);

		std::tie(msgs, lastIdx) = fh->getBatch(0);

		QCOMPARE(msgs.size(), 2);
		QCOMPARE(msgs.at(0).cast<protocol::Chat>().message(), QString("test2"));
		QCOMPARE(lastIdx, 2);

		std::tie(msgs, lastIdx) = fh->getBatch(1);

		QCOMPARE(msgs.size(), 1);
		QCOMPARE(msgs.at(0).cast<protocol::Chat>().message(), QString("test3"));
		QCOMPARE(lastIdx, 2);

		std::tie(msgs, lastIdx) = fh->getBatch(2);

		QCOMPARE(msgs.size(), 0);
		QCOMPARE(lastIdx, 2);
	}

	// Tolerate truncated messages
	void testTruncation()
	{
		QString file = makeTestRecording();

		// Cut off the end of the recording
		QString recfile = m_dir.absoluteFilePath(file);
		recfile.replace(".session", ".dprec");
		QFile rf(recfile);
		QVERIFY(rf.resize(rf.size() - 3));

		// The first two messages should still be readable
		std::unique_ptr<FiledHistory> fh { FiledHistory::load(m_dir.absoluteFilePath(file)) };

		QList<protocol::MessagePtr> msgs;
		int lastIdx;

		std::tie(msgs, lastIdx) = fh->getBatch(-1);

		QCOMPARE(msgs.size(), 2);
		QCOMPARE(msgs.at(0).cast<protocol::Chat>().message(), QString("test1"));
		QCOMPARE(msgs.at(1).cast<protocol::Chat>().message(), QString("test2"));
		QCOMPARE(lastIdx, 1);
	}

	// Check if history reset is handled correctly
	void testReset()
	{
		QString file = makeTestRecording();
		std::unique_ptr<FiledHistory> fh { FiledHistory::load(m_dir.absoluteFilePath(file)) };

		auto testMsg = protocol::MessagePtr(new protocol::Chat(1, 0, 0, QByteArray("test0")));

		fh->addMessage(testMsg);
		fh->addMessage(testMsg);
		fh->addMessage(testMsg);

		QCOMPARE(fh->lastIndex(), 5);
		QCOMPARE(fh->sizeInBytes(), uint(testMsg->length()*6));

		QList<protocol::MessagePtr> msgs;
		int lastIdx;
		std::tie(msgs, lastIdx) = fh->getBatch(-1);
		QCOMPARE(msgs.size(), 6);
		QCOMPARE(lastIdx, 5);
		QVERIFY(msgs.at(3).equals(testMsg));

		QList<protocol::MessagePtr> newContent;
		newContent << testMsg;
		newContent << testMsg;

		fh->reset(newContent);

		QCOMPARE(fh->lastIndex(), 7);
		QCOMPARE(fh->sizeInBytes(), uint(testMsg->length()*2));

		std::tie(msgs, lastIdx) = fh->getBatch(1); // any index below firstIndex() should work the same
		QCOMPARE(msgs.size(), 2);
		QCOMPARE(lastIdx, 7);
		QVERIFY(msgs.at(0).equals(testMsg));
	}

	void testBlockEnd()
	{
		QString file = makeTestRecording();
		std::unique_ptr<FiledHistory> fh { FiledHistory::load(m_dir.absoluteFilePath(file)) };

		QCOMPARE(fh->lastIndex(), 2);

		fh->closeBlock();

		auto testMsg = protocol::MessagePtr(new protocol::Chat(1, 0, 0, QByteArray("test0")));

		fh->addMessage(testMsg);
		fh->addMessage(testMsg);

		QCOMPARE(fh->lastIndex(), 4);

		// First batch should contain the first block
		QList<protocol::MessagePtr> msgs;
		int lastIdx;
		std::tie(msgs, lastIdx) = fh->getBatch(-1);
		QCOMPARE(msgs.size(), 3);
		QCOMPARE(lastIdx, 2);
		QCOMPARE(msgs.last().cast<protocol::Chat>().message(), QString("test3"));

		// Second batch
		std::tie(msgs, lastIdx) = fh->getBatch(lastIdx);
		QCOMPARE(msgs.size(), 2);
		QCOMPARE(lastIdx, 4);
		QCOMPARE(msgs.first().cast<protocol::Chat>().message(), QString("test0"));

		// There is no third batch
		std::tie(msgs, lastIdx) = fh->getBatch(lastIdx);
		QCOMPARE(msgs.size(), 0);
		QCOMPARE(lastIdx, 4);

		// Until now
		fh->closeBlock();
		fh->closeBlock(); // closeBlock should be idempotent
		fh->addMessage(testMsg);

		std::tie(msgs, lastIdx) = fh->getBatch(lastIdx);
		QCOMPARE(msgs.size(), 1);
		QCOMPARE(lastIdx, 5);

		// Having an empty block at the end shouldn't have any effect
		fh->closeBlock();
		std::tie(msgs, lastIdx) = fh->getBatch(lastIdx-1);
		QCOMPARE(msgs.size(), 1);
		QCOMPARE(lastIdx, 5);
	}

	void testUserLeave()
	{
		QUuid id = QUuid::createUuid();
		{
			std::unique_ptr<FiledHistory> fh { FiledHistory::startNew(m_dir, id, QString(), protocol::ProtocolVersion::current(), "test") };

			fh->addMessage(protocol::MessagePtr(new protocol::UserJoin(1, 0, QByteArray("u1"), QByteArray())));
			fh->addMessage(protocol::MessagePtr(new protocol::UserJoin(2, 0, QByteArray("u2"), QByteArray())));
			fh->addMessage(protocol::MessagePtr(new protocol::Chat(1, 0, 0, QByteArray("test1"))));
			fh->addMessage(protocol::MessagePtr(new protocol::UserLeave(2)));
		}
		{
			std::unique_ptr<FiledHistory> fh { FiledHistory::load(m_dir.absoluteFilePath(FiledHistory::journalFilename(id))) };
			QVERIFY(fh.get());

			QList<protocol::MessagePtr> msgs;
			int lastIdx;
			std::tie(msgs, lastIdx) = fh->getBatch(-1);
			QCOMPARE(msgs.size(), 5);
			QCOMPARE(msgs.last()->type(), protocol::MSG_USER_LEAVE);
			QCOMPARE(msgs.last()->contextId(), uint8_t(1));
		}
	}

private:
	QString makeTestRecording()
	{
		QUuid id = QUuid::createUuid();
		std::unique_ptr<FiledHistory> fh { FiledHistory::startNew(m_dir, id, QString(), protocol::ProtocolVersion::current(), "test") };

		fh->addMessage(protocol::MessagePtr(new protocol::Chat(1, 0, 0, QByteArray("test1"))));
		fh->addMessage(protocol::MessagePtr(new protocol::Chat(1, 0, 0, QByteArray("test2"))));
		fh->addMessage(protocol::MessagePtr(new protocol::Chat(1, 0, 0, QByteArray("test3"))));

		return FiledHistory::journalFilename(id);
	}

private:
	QTemporaryDir m_tempdir;
	QDir m_dir;
};


QTEST_MAIN(TestFiledHistory)
#include "filedhistory.moc"

