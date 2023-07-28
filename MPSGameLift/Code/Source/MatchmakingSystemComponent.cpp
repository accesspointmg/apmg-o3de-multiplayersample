/*
 * Copyright (c) Contributors to the Open 3D Engine Project. For complete copyright and license terms please see the LICENSE at the root of this distribution.
 * 
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "MatchmakingSystemComponent.h"

#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/RTTI/BehaviorContext.h>

#include <AWSCoreBus.h>
#include <ResourceMapping/AWSResourceMappingBus.h>

#include <Framework/Error.h>
#include <Framework/ServiceRequestJob.h>


namespace MPSGameLift
{
    namespace ServiceAPI
    {
        struct PlayerSkill
        {
            //! Storage for a matchmaking player's skill level as defined by MultiplayerSample's matchmaking ruleset.
            //! Capturing values returned by GameLift's MatchmakingTicket::Players::PlayerAttributes::skill response
            //! https://docs.aws.amazon.com/gamelift/latest/apireference/API_Player.html
            bool OnJsonKey(const char* key, AWSCore::JsonReader& reader)
            {
                // Player skill is a number
                // FlexMatch uses "N" (number) for interpreting how rules are logically compared
                if (strcmp(key, "N") == 0)
                {
                    return reader.Accept(skill);
                }
                return reader.Ignore();
            }

            int skill;
        };

        //! A collection of key:value pairs containing player information for use in matchmaking
        //! Capturing values returned by GameLift's MatchmakingTicket::Players::PlayerAttributes response
        //! https://docs.aws.amazon.com/gamelift/latest/apireference/API_Player.html
        struct PlayerAttributes
        {
            bool OnJsonKey(const char* key, AWSCore::JsonReader& reader)
            {
                if (strcmp(key, "skill") == 0)
                {
                    return reader.Accept(skill);
                }
                return reader.Ignore();
            }

            PlayerSkill skill;
        };

        //! Struct for storing a player's regional latency map
        //! Capturing values returned by GameLift's MatchmakingTicket::Players::LatencyInMs response
        //! https://docs.aws.amazon.com/gamelift/latest/apireference/API_Player.html
        struct Latencies
        {
            bool OnJsonKey(const char* key, AWSCore::JsonReader& reader)
            {
                return reader.Accept(latencies[key]);
            }

            AZStd::unordered_map<AZStd::string, int> latencies;
        };

        //! Struct for storing a player in matchmaking
        //! Capturing values returned by GameLift's MatchmakingTicket::Players response
        //! https://docs.aws.amazon.com/gamelift/latest/apireference/API_Player.html
        struct Player
        {
            bool OnJsonKey(const char* key, AWSCore::JsonReader& reader)
            {
                if (strcmp(key, "LatencyInMs") == 0)
                {
                    return reader.Accept(latencies);
                }
                if (strcmp(key, "PlayerId") == 0)
                {
                    return reader.Accept(playerId);
                }
                if (strcmp(key, "Team") == 0)
                {
                    return reader.Accept(team);
                }
                if (strcmp(key, "PlayerAttributes") == 0)
                {
                    return reader.Accept(playerAttributes);
                }
                return reader.Ignore();
            }

            AZStd::string playerId;
            AZStd::string team;
            Latencies latencies;
            PlayerAttributes playerAttributes;
        };

        //! Struct for storing the success response.
        //! Capturing ticket-id and players data provided by GameLift's Matchmaking response
        //! https://docs.aws.amazon.com/gamelift/latest/apireference/API_MatchmakingTicket.html
        struct RequestMatchmakingResponse
        {
            bool OnJsonKey(const char* key, AWSCore::JsonReader& reader)
            {
                if (strcmp(key, "TicketId") == 0)
                {
                    return reader.Accept(ticketId);
                }
                if (strcmp(key, "Players") == 0)
                {
                    return reader.Accept(players);
                }

                return reader.Ignore();
            }

            AZStd::string ticketId;
            AZStd::vector<Player> players;
        };

        // Service RequestJobs
        AWS_FEATURE_GEM_SERVICE(MPSGameLift);

        //! GET request to place a matchmaking request "/requestmatchmaking".
        class RequestMatchmaking
            : public AWSCore::ServiceRequest
        {
        public:
            SERVICE_REQUEST(MPSGameLift, HttpMethod::HTTP_GET, "");

            struct Parameters
            {
                bool BuildRequest(AWSCore::RequestBuilder& request)
                {
                    return request.WriteJsonBodyParameter(*this);
                }

                bool WriteJson([[maybe_unused]]AWSCore::JsonWriter& writer) const
                {
                    return true;
                }
            };

            RequestMatchmakingResponse result;
            AWSCore::Error error;
            Parameters parameters; //! Request parameter.
        };

        using MPSRequestMatchmakingJob = AWSCore::ServiceRequestJob<RequestMatchmaking>;
    }  // ServiceAPI

    void MatchmakingSystemComponent::Activate()
    {
        AZ::Interface<IMatchmaking>::Register(this);
    }

    void MatchmakingSystemComponent::Deactivate()
    {
        AZ::Interface<IMatchmaking>::Unregister(this);
    }

    void MatchmakingSystemComponent::Reflect(AZ::ReflectContext* context)
    {
        if (auto serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<MatchmakingSystemComponent, AZ::Component>()
                ->Version(0)
                ;
        }
    }

    void MatchmakingSystemComponent::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided)
    {
        provided.push_back(AZ_CRC_CE("MPSGameLiftMatchmaking"));
    }

   bool MatchmakingSystemComponent::RequestMatch(const RegionalLatencies& regionalLatencies)
    {
        if (!m_ticketId.empty())
        {
            AZ_Warning("MatchmakingSystemComponent", false, "Ticket already exists %s", m_ticketId.c_str())
            return true;
        }

        // Digest latencies for the HTTP GET parameter
        AZ_Assert(!regionalLatencies.empty(), "IMatchmaking::RequestMatch failed! Client needs to provide regional latencies in order to determine the best server to join!")
        AZStd::string httpLatenciesParam;
        for (auto const& [region, latencyMs] : regionalLatencies)
        {
            httpLatenciesParam += AZStd::string::format("%s_%lld ", region.c_str(), latencyMs.count());
        }

        httpLatenciesParam.pop_back();  // pop the trailing white-space

        // Set API endpoint and region
        ServiceAPI::MPSRequestMatchmakingJob::Config* config = ServiceAPI::MPSRequestMatchmakingJob::GetDefaultConfig();
        AZStd::string actualRegion;
        AWSCore::AWSResourceMappingRequestBus::BroadcastResult(actualRegion, &AWSCore::AWSResourceMappingRequests::GetDefaultRegion);

        AZStd::string restApi;
        AWSCore::AWSResourceMappingRequestBus::BroadcastResult(restApi, &AWSCore::AWSResourceMappingRequests::GetResourceNameId, "MPSMatchmaking");
        config->region = actualRegion.c_str();
        config->endpointOverride = AZStd::string::format("https://%s.execute-api.%s.amazonaws.com/%s?latencies=%s",
            restApi.c_str(), actualRegion.c_str(), "Prod/requestmatchmaking", httpLatenciesParam.c_str()).c_str();

        // Request serverless backend to make match
        ServiceAPI::MPSRequestMatchmakingJob* requestJob = ServiceAPI::MPSRequestMatchmakingJob::Create(
            [this](ServiceAPI::MPSRequestMatchmakingJob* successJob)
            {
                m_ticketId = successJob->result.ticketId;
            },
            []([[maybe_unused]] ServiceAPI::MPSRequestMatchmakingJob* failJob)
            {
                AZ_Error("MatchmakingSystemComponent", false, "Unable to request match error: %s", failJob->error.message.c_str());
            },
            config);

        requestJob->Start();
        return true;
    }
}