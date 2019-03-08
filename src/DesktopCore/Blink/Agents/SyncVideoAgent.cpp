#include "SyncVideoAgent.h"

#include "Utils\Patterns\PublisherSubscriber\Broker.h"
#include "../../Network/Events.h"
#include "..\..\Network\Model\Credentials.h"
#include "..\..\System\Services\IniFileService.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <chrono>
#include <thread>

namespace desktop { namespace core { namespace agent {

	SyncVideoAgent::SyncVideoAgent(std::unique_ptr<service::IDownloadFileService> downloadService,
									std::unique_ptr<service::HTTPClientService> clientService,
									std::unique_ptr<service::ApplicationDataService> applicationService,
									std::unique_ptr<service::IniFileService> iniFileService)
	: m_ioService()
	, m_iniFileService(std::move(iniFileService))
	, m_downloadService(std::move(downloadService))
	, m_clientService(std::move(clientService))
	, m_applicationService(std::move(applicationService))
	{
		{
			auto documents = m_applicationService->getMyDocuments();
			m_seconds = m_iniFileService->get<unsigned int>(documents + "Blink.ini", "Synchronize", "Interval", 60);

			m_timer = std::make_unique<boost::asio::deadline_timer>(m_ioService, boost::posix_time::seconds(m_seconds));

			m_outFolder = m_iniFileService->get<std::string>(documents + "Blink.ini", "Synchronize", "Output", documents + "Download\\Videos\\");

			boost::filesystem::create_directories(m_outFolder);
		}

		m_subscriber.subscribe([this](const desktop::core::utils::patterns::Event& rawEvt)
		{
			const auto& evt = static_cast<const core::events::CredentialsEvent&>(rawEvt);

			m_credentials = std::make_unique<model::Credentials>(evt.m_credentials);

			if (!m_enabled)
			{
				m_enabled = true;
				armTimer(1);

				boost::thread t(boost::bind(&boost::asio::io_service::run, &m_ioService));
				m_backgroundThread.swap(t);
			}
		}, events::CREDENTIALS_EVENT);
	}

	SyncVideoAgent::~SyncVideoAgent()
	{
		m_enabled = false;
		m_timer->cancel();
		m_backgroundThread.join();
		m_ioService.reset();
	}

	std::string SyncVideoAgent::getLastUpdateTimestamp() const
	{
		auto documents = m_applicationService->getMyDocuments();
		auto timestamp = m_iniFileService->get<std::string>(documents + "Blink.ini", "Synchronize", "LastUpdate", "-999999999-01-01T00:00:00+00:00");

		return timestamp;
	}

	void SyncVideoAgent::setLastUpdateTimestamp() const
	{
		time_t rawtime;
		time(&rawtime);

		struct tm timeinfo;

		localtime_s(&timeinfo, &rawtime);

		std::stringstream ss;
		ss << timeinfo.tm_year + 1900 << "-" << std::setfill('0') << std::setw(2) << timeinfo.tm_mon + 1 << "-"
			<< std::setfill('0') << std::setw(2) << timeinfo.tm_mday
			<< "T" << std::setfill('0') << std::setw(2) << timeinfo.tm_hour << ":"
			<< std::setfill('0') << std::setw(2) << timeinfo.tm_min << ":"
			<< std::setfill('0') << std::setw(2) << timeinfo.tm_sec << "+00:00";
		
		setLastUpdateTimestamp(ss.str());
	}

	void SyncVideoAgent::setLastUpdateTimestamp(const std::string& timestamp) const
	{
		auto documents = m_applicationService->getMyDocuments();

		m_iniFileService->set<std::string>(documents + "Blink.ini", "Synchronize", "LastUpdate", timestamp);
	}

	void SyncVideoAgent::execute()
	{
		if (m_enabled && m_credentials)
		{
			std::map<std::string, std::string> videos;

			std::string lastUpdate = getLastUpdateTimestamp();

			getVideos(videos, "/api/v2/videos/changed?since=" + lastUpdate, 1);

			if (videos.size() > 0)
			{
				auto documents = m_applicationService->getMyDocuments();
				unsigned int sleep = m_iniFileService->get<unsigned int>(documents + "Blink.ini", "Synchronize", "Sleep", 20);

				std::map<std::string, std::string> requestHeaders;
				requestHeaders["token_auth"] = m_credentials->m_token;

				std::string months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

				for (auto &video : videos)
				{
					std::stringstream ss(video.first);

					std::string year, month, day;
					getline(ss, year, '-'); getline(ss, month, '-'); getline(ss, day, 'T');

					int monthNumber = std::stoi(month);

					if (monthNumber <= 12)
					{
						month = months[monthNumber - 1];
					}

					auto fileName = boost::filesystem::path(video.second);

					auto folder = m_outFolder + year + "\\" + month + "\\" + day + "\\";
					auto target = folder + fileName.filename().string();

					if (!boost::filesystem::exists(target))
					{
						boost::filesystem::create_directories(folder);

						try
						{
							m_downloadService->download(m_credentials->m_host, video.second, requestHeaders, target);
							setLastUpdateTimestamp(video.first);

							std::this_thread::sleep_for(std::chrono::seconds{ sleep });
						}
						catch (...)
						{
							break;
						}
					}
					else
					{
						setLastUpdateTimestamp(video.first);
					}
				}
			}
		}
	}

	void SyncVideoAgent::getVideos(std::map<std::string, std::string>& videos, const std::string& path, unsigned int page) const
	{
		std::map<std::string, std::string> requestHeaders, responseHeaders;
		std::string content;
		unsigned int status;

		requestHeaders["token_auth"] = m_credentials->m_token;

		std::stringstream ss;
		ss << path << "&page=" << page;

		if (m_clientService->send(m_credentials->m_host, m_credentials->m_port, ss.str(), requestHeaders, responseHeaders, content, status))
		{
			std::stringstream contentSS(content);

			try
			{
				boost::property_tree::ptree tree;
				boost::property_tree::json_parser::read_json(contentSS, tree);
				
				auto videosTag = tree.get_child("videos");

				if (videosTag.size() > 0)
				{
					for (auto &video : videosTag)
					{
						if (!video.second.get_child("deleted").get_value<bool>())
						{
							videos[video.second.get_child("created_at").get_value<std::string>()] = video.second.get_child("address").get_value<std::string>();
						}
					}

					getVideos(videos, path, page + 1);
				}
			}
			catch (...)
			{
			
			}
		}
	}

	void SyncVideoAgent::armTimer(unsigned int seconds)
	{
		m_timer->expires_from_now(boost::posix_time::seconds(seconds));

		m_timer->async_wait([&](const boost::system::error_code& ec)
		{
			execute();
			armTimer(m_seconds);
		});
	}
}}}