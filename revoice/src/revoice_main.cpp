#include "precompiled.h"
//#include "SKP_Silk_SigProc_FIX.h"
#include "soxr.h"
//std::unordered_map<size_t, std::unordered_map<size_t, bool>> sended_voice;

void SV_DropClient_hook(IRehldsHook_SV_DropClient *chain, IGameClient *cl, bool crash, const char *msg)
{
	CRevoicePlayer *plr = GetPlayerByClientPtr(cl);

	plr->OnDisconnected();

	chain->callNext(cl, crash, msg);
}

void CvarValue2_PreHook(const edict_t *pEnt, int requestID, const char *cvarName, const char *cvarValue)
{
	CRevoicePlayer *plr = GetPlayerByEdict(pEnt);
	if (plr->GetRequestId() != requestID) {
		RETURN_META(MRES_IGNORED);
	}

	const char *lastSeparator = strrchr(cvarValue, ',');
	if (lastSeparator)
	{
		int buildNumber = atoi(lastSeparator + 1);
		if (buildNumber > 4554) {
			plr->SetCodecType(vct_opus);
		}
	}

	RETURN_META(MRES_IGNORED);
}

bool TranscodeVoice(CRevoicePlayer *srcPlayer, std::vector<char> srcBuff, IVoiceCodec *srcCodec, std::vector<std::pair<IVoiceCodec*, std::vector<char>*>> codecBuffers)
{
	static char originalBuf[32768];
	static char resampledBuf[32768];
	static char transCodedBuf[32768];
	char* decodedBuf;
	int numDecodedSamples;
	size_t origDecodedSamples = srcCodec->Decompress(srcBuff.data(), srcBuff.size(), originalBuf, sizeof(originalBuf));
	if (origDecodedSamples <= 0) {
		return false;
	}
  	g_OnDecompress(srcPlayer->GetClient()->GetId(), srcCodec->SampleRate(), reinterpret_cast<uint8_t*>(originalBuf), reinterpret_cast<size_t*>(&origDecodedSamples));
	for(auto &c : codecBuffers)
	{
		decodedBuf = originalBuf;
		numDecodedSamples = origDecodedSamples;
		if(c.first->SampleRate() != srcCodec->SampleRate())
		{
			size_t inSampleRate = srcCodec->SampleRate();
			size_t outSampleRate = c.first->SampleRate();
			size_t outSampleCount = size_t(ceil(double(numDecodedSamples) * (double(outSampleRate) / double(inSampleRate))));
			size_t odone;
			soxr_runtime_spec_t _soxrRuntimeSpec = soxr_runtime_spec(2);
			soxr_quality_spec_t _soxrQualitySpec = soxr_quality_spec(SOXR_MQ, 0);
			soxr_io_spec_t iospec = soxr_io_spec(SOXR_INT16, SOXR_INT16);
			soxr_oneshot(inSampleRate, outSampleRate, 1, decodedBuf, numDecodedSamples, NULL, resampledBuf, sizeof(resampledBuf), &odone,
			&iospec, &_soxrQualitySpec, &_soxrRuntimeSpec);
			numDecodedSamples = odone;
			decodedBuf = resampledBuf;
		}	
		int writted = c.first->Compress(decodedBuf, numDecodedSamples, transCodedBuf,  sizeof(transCodedBuf), false);
		if(writted <= 0)
		{			
			return false;
		}
		c.second->insert(c.second->end(), transCodedBuf, transCodedBuf + writted);
	}
	return true;
}

int EncodeVoice(size_t senderId, char* srcBuf, int srcBufLen, IVoiceCodec* dstCodec, char* dstBuf, int dstBufSize, bool debug = false)
{
	g_OnDecompress(senderId, dstCodec->SampleRate(), reinterpret_cast<uint8_t*>(srcBuf), reinterpret_cast<size_t*>(&srcBufLen));
	int compressedSize = dstCodec->Compress(srcBuf, srcBufLen, dstBuf, dstBufSize, false);
	if (compressedSize <= 0) {
		return 0;
	}	
	return compressedSize;
}

void SV_ParseVoiceData_emu(IGameClient *cl)
{
	std::vector<char> recvBuff;
	std::vector<char> speexBuf;
	std::vector<char> steamBuf;

	int silkDataLen = 0;
	int speexDataLen = 0;
	uint16_t nDataLength = g_RehldsFuncs->MSG_ReadShort();

	if (nDataLength > 8192 /* todo: make value tweakable */ ) {
		g_RehldsFuncs->DropClient(cl, FALSE, "Invalid voice data\n");
		return;
	}
	recvBuff.resize(nDataLength);
	g_RehldsFuncs->MSG_ReadBuf(nDataLength, recvBuff.data());

	if (g_pcv_sv_voiceenable->value == 0.0f) {
		return;
	}

	CRevoicePlayer *srcPlayer = GetPlayerByClientPtr(cl);
	if (srcPlayer->IsBlocked())
		return;
	if (!srcPlayer->IsSpeaking())
	{
		srcPlayer->Speak();
		g_OnClientStartSpeak(cl->GetId());
	}
	srcPlayer->SetLastVoiceTime(g_RehldsSv->GetTime());
	srcPlayer->IncreaseVoiceRate(nDataLength);


	switch (srcPlayer->GetCodecType())
	{
		case vct_silk:
		{
			if(!TranscodeVoice(srcPlayer, recvBuff, srcPlayer->GetSilkCodec(), {{srcPlayer->GetOpusCodec(), &steamBuf}, {srcPlayer->GetSpeexCodec(), &speexBuf} }))
			{
				return;
			}
			break;
		}
		case vct_opus:
		{
			if(!TranscodeVoice(srcPlayer, recvBuff, srcPlayer->GetOpusCodec(), {{srcPlayer->GetOpusCodec(), &steamBuf}, {srcPlayer->GetSpeexCodec(), &speexBuf} }))
			{
				return;
			}
			break;
		}
		case vct_speex:
		{
			if(!TranscodeVoice(srcPlayer, recvBuff, srcPlayer->GetSpeexCodec(), {{srcPlayer->GetOpusCodec(), &steamBuf}, {srcPlayer->GetSpeexCodec(), &speexBuf} }))
			{
				return;
			}
			break;
		}
		default:
			return;
	}

	if (srcPlayer->IsMuted())
		return;
	
	int maxclients = g_RehldsSvs->GetMaxClients();
	for (int i = 0; i < maxclients; i++)
	{
		CRevoicePlayer *dstPlayer = &g_Players[i];
		IGameClient *dstClient = dstPlayer->GetClient();

		if (!((1 << i) & cl->GetVoiceStream(0)) && dstPlayer != srcPlayer)
			continue;

		if (!dstClient->IsActive() && !dstClient->IsConnected() && dstPlayer != srcPlayer)
			continue;
				
		if (g_mute_map[i][cl->GetId() - 1] || g_mute_map[i][-1])
			continue;
		
		char *sendBuf;
		int nSendLen;
		switch (dstPlayer->GetCodecType())
		{
		case vct_silk:
		case vct_opus:
			sendBuf = steamBuf.data();
			nSendLen = steamBuf.size();
			break;
		case vct_speex:
			sendBuf = speexBuf.data();
			nSendLen = speexBuf.size();
			break;
		default:
			sendBuf = nullptr;
			nSendLen = 0;
			break;
		}

		if (sendBuf == nullptr || nSendLen == 0)
			continue;

		if (dstPlayer == srcPlayer && !dstClient->GetLoopback())
			nSendLen = 0;

		
		sizebuf_t *dstDatagram = dstClient->GetDatagram();
		if (dstDatagram->cursize + nSendLen + 4 < dstDatagram->maxsize) {
			g_RehldsFuncs->MSG_WriteByte(dstDatagram, svc_voicedata);
			g_RehldsFuncs->MSG_WriteByte(dstDatagram, cl->GetId());
			g_RehldsFuncs->MSG_WriteShort(dstDatagram, nSendLen);
			g_RehldsFuncs->MSG_WriteBuf(dstDatagram, nSendLen, sendBuf);
		}
	}
}

void Rehlds_HandleNetCommand(IRehldsHook_HandleNetCommand *chain, IGameClient *cl, int8 opcode)
{
	const int clc_voicedata = 8;
	if (opcode == clc_voicedata) {
		SV_ParseVoiceData_emu(cl);
		return;
	}

	chain->callNext(cl, opcode);
}

qboolean ClientConnect_PostHook(edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128])
{
	CRevoicePlayer *plr = GetPlayerByEdict(pEntity);
	plr->OnConnected();

	RETURN_META_VALUE(MRES_IGNORED, TRUE);
}

void ServerActivate_PostHook(edict_t* pEdictList, int edictCount, int clientMax)
{
	// It is bad because it sends to hltv
	MESSAGE_BEGIN(MSG_INIT, SVC_STUFFTEXT);
	WRITE_STRING("VTC_CheckStart\n");
	MESSAGE_END();
	MESSAGE_BEGIN(MSG_INIT, SVC_STUFFTEXT);
	WRITE_STRING("vgui_runscript\n");
	MESSAGE_END();
	MESSAGE_BEGIN(MSG_INIT, SVC_STUFFTEXT);
	WRITE_STRING("VTC_CheckEnd\n");
	MESSAGE_END();
	
	Revoice_Exec_Config();
	SET_META_RESULT(MRES_IGNORED);
}
void ServerDeactivate_PostHook()
{
	g_mute_map.clear();
	g_audio_waves.clear();
	SET_META_RESULT(MRES_IGNORED);
}

void SV_WriteVoiceCodec_hooked(IRehldsHook_SV_WriteVoiceCodec *chain, sizebuf_t *sb)
{
	IGameClient *cl = g_RehldsFuncs->GetHostClient();
	CRevoicePlayer *plr = GetPlayerByClientPtr(cl);

	switch (plr->GetCodecType())
	{
		case vct_silk:
		case vct_opus:
		case vct_speex:
		{
			g_RehldsFuncs->MSG_WriteByte(sb, svc_voiceinit);
			g_RehldsFuncs->MSG_WriteString(sb, "voice_speex");	// codec id
			g_RehldsFuncs->MSG_WriteByte(sb, 5);				// quality
			break;
		}
		default:
			LCPrintf(true, "SV_WriteVoiceCodec() called on client(%d) with unknown voice codec\n", cl->GetId());
			break;
	}
}

bool Revoice_Load()
{
	if (!Revoice_Utils_Init())
		return false;

	if (!Revoice_RehldsApi_Init()) {
		LCPrintf(true, "Failed to locate REHLDS API\n");
		return false;
	}

	if (!Revoice_ReunionApi_Init())
		return false;

	Revoice_Init_Cvars();
	Revoice_Init_Config();
	Revoice_Init_Players();

	if (!Revoice_Main_Init()) {
		LCPrintf(true, "Initialization failed\n");
		return false;
	}	
	g_RehldsApi->GetFuncs()->RegisterPluginApi("VoiceTranscoder", &g_voiceTranscoderAPI);
	g_RehldsApi->GetFuncs()->RegisterPluginApi("RevoicePlus", &g_revoiceAPI);

	return true;
}

void PlayerPreThink(edict_t* pEntity)
{
	/*
	if (pEntity)
	{
		sended_voice[ENTINDEX(pEntity) - 1].clear();
	}
	*/
}
short int mix_sample(short int sample1, short int sample2) {
	const int32_t result(static_cast<int32_t>(sample1) + static_cast<int32_t>(sample2));
	typedef std::numeric_limits<short int> Range;
	if (Range::max() < result)
		return Range::max();
	else if (Range::min() > result)
		return Range::min();
	else
		return result;
}

void StartFrame_PostHook()
{
	
	static char silkBuf[4096];
	static char speexBuf[4096];

	int maxclients = g_RehldsSvs->GetMaxClients();
	for (int i = 0; i < maxclients; i++)
	{
		CRevoicePlayer* player = &g_Players[i];
		IGameClient* client = player->GetClient();


		if (!client->IsActive() && !client->IsConnected())
			continue;

		if (!player->CheckSpeaking() && player->IsSpeaking())
		{
			player->SpeakDone();
			g_OnClientStopSpeak(i);
		}
	}	
	auto end = g_audio_waves.end();
	std::unordered_set<uint32_t> handled_sounds;
	auto now = std::chrono::high_resolution_clock::now();
	for (auto sound = g_audio_waves.begin(); sound != end; sound++)
	{
		if (handled_sounds.find(sound->first) != handled_sounds.end())
		{
			continue;
		}
		if (sound->second.state != audio_wave_play::PLAY_PLAY)
		{
			continue;
		}

		if (sound->second.flPlayPos16k >= sound->second.wave16k->length() && sound->second.flPlayPos8k >= sound->second.wave8k->length())
		{
			handled_sounds.insert(sound->first);
			g_OnSoundComplete(sound->first);
			// in case some forward delete sound from map
			if (g_audio_waves.empty())
			{
				break;
			}
			end = g_audio_waves.end();
			sound = g_audio_waves.begin();
			continue;
		}
		
		if (now > sound->second.nextSend16k && sound->second.flPlayPos16k < sound->second.wave16k->length())
		{

			std::unordered_set<uint32_t> need_mix;
			need_mix.insert(sound->first);
			for (auto sound2 = g_audio_waves.begin(); sound2 != g_audio_waves.end(); sound2++)
			{
				if (sound2->second.state != audio_wave_play::PLAY_PLAY)
				{
					continue;
				}

				if (sound != sound2 && sound->second.senderClientIndex == sound2->second.senderClientIndex)
				{
					need_mix.insert(sound2->first);
				}
			}
			size_t n_samples = 0;
			uint8_t* sample_buffer;
			size_t speexDataLen = 0;
			size_t silkDataLen = 0;
			size_t full_length;
			std::vector<uint16_t> mix;

			if (need_mix.size() > 1)
			{
				for (auto& i : need_mix)
				{
					handled_sounds.insert(i);
					size_t mix_samples = 0;
					auto& s = g_audio_waves[i];
					full_length = s.wave16k->sample_rate() / 50;
					auto ptr = (uint16_t*)s.wave16k->get_samples(full_length, &mix_samples);
					s.nextSend16k = now + std::chrono::milliseconds((size_t)(mix_samples/(double)s.wave16k->sample_rate()*1000)) - std::chrono::microseconds((size_t)(1e6 / s.wave16k->sample_rate()));
					if (mix.empty())
					{
						mix = { ptr , ptr + mix_samples };
					}
					else
					{
						std::vector<uint16_t> vec = { ptr , ptr + mix_samples };
						for (size_t j = 0; j < mix.size(); j++)
						{
							if (j >= vec.size())
							{
								break;
							}
							mix[j] = mix_sample(mix[j], vec[j]);
						}
						if (vec.size() > mix.size())
						{
							mix.insert(mix.end(), vec.begin() + mix.size(), vec.end());
						}
					}
					if (s.auto_delete)
					{
						s.wave16k->clear_frames(mix_samples);
					}
					else
					{
						s.flPlayPos16k += (float)mix_samples / s.wave16k->sample_rate();
						s.wave16k->seek(mix_samples, audio_wave::seekCurr);
					}
				}				
				silkDataLen = EncodeVoice(sound->second.senderClientIndex, reinterpret_cast<char*>(mix.data()), mix.size(), sound->second.SteamCodec.get(), silkBuf, sizeof(silkBuf));
			}
			else
			{

				handled_sounds.insert(sound->first);
				full_length = sound->second.wave16k->sample_rate() / 50;
				sample_buffer = sound->second.wave16k->get_samples(full_length, &n_samples);
				sound->second.nextSend16k = now + std::chrono::milliseconds((size_t)(n_samples/(double)sound->second.wave16k->sample_rate()*1000)) - std::chrono::microseconds((size_t)(1e6 / sound->second.wave16k->sample_rate()));
				
				
				silkDataLen = EncodeVoice(sound->second.senderClientIndex, reinterpret_cast<char*>(sample_buffer), n_samples, sound->second.SteamCodec.get(), silkBuf, sizeof(silkBuf));
				if (sound->second.auto_delete)
				{
					sound->second.wave16k->clear_frames(n_samples);
				}
				else
				{
					sound->second.flPlayPos16k += (float)n_samples / sound->second.wave16k->sample_rate();
					sound->second.wave16k->seek(n_samples, audio_wave::seekCurr);
				}
			}

			
			for (int i = 0; i < maxclients; i++)
			{
				CRevoicePlayer* player = &g_Players[i];
				IGameClient* client = player->GetClient();
				if (!client->IsActive() && !client->IsConnected())
					continue;

				if (g_mute_map[i][sound->second.senderClientIndex] || g_mute_map[i][-1])
					continue;
				if (sound->second.receivers.find(-1) == sound->second.receivers.end() && sound->second.receivers.find(i) == sound->second.receivers.end())
				{
					continue;
				}

				if(player->GetCodecType() == vct_speex)
				{
					continue;
				}

				char* sendBuf = silkBuf;
				int nSendLen = silkDataLen;
				if (nSendLen == 0 || sendBuf == nullptr)
					continue;
				sizebuf_t* dstDatagram = client->GetDatagram();
				if (dstDatagram->cursize + nSendLen + 4 < dstDatagram->maxsize) {
					g_RehldsFuncs->MSG_WriteByte(dstDatagram, svc_voicedata);
					g_RehldsFuncs->MSG_WriteByte(dstDatagram, sound->second.senderClientIndex);
					g_RehldsFuncs->MSG_WriteShort(dstDatagram, nSendLen);
					g_RehldsFuncs->MSG_WriteBuf(dstDatagram, nSendLen, sendBuf);
				}
			}
		}
		if (sound->second.flPlayPos16k >= sound->second.wave16k->length() && sound->second.flPlayPos8k >= sound->second.wave8k->length())
		{
			handled_sounds.insert(sound->first);
			g_OnSoundComplete(sound->first);
			// in case some forward delete sound from map
			if (g_audio_waves.empty())
			{
				break;
			}
			end = g_audio_waves.end();
			sound = g_audio_waves.begin();
			continue;
		}

		if (now > sound->second.nextSend8k && sound->second.flPlayPos8k < sound->second.wave8k->length())
		{

			std::unordered_set<uint32_t> need_mix;
			need_mix.insert(sound->first);
			for (auto sound2 = g_audio_waves.begin(); sound2 != g_audio_waves.end(); sound2++)
			{
				if (sound2->second.state != audio_wave_play::PLAY_PLAY)
				{
					continue;
				}

				if (sound != sound2 && sound->second.senderClientIndex == sound2->second.senderClientIndex)
				{
					need_mix.insert(sound2->first);
				}
			}
			size_t n_samples = 0;
			uint8_t* sample_buffer;
			size_t speexDataLen = 0;
			size_t silkDataLen = 0;
			size_t full_length;
			std::vector<uint16_t> mix;

			if (need_mix.size() > 1)
			{
				for (auto& i : need_mix)
				{
					using namespace std::chrono_literals;
					handled_sounds.insert(i);
					size_t mix_samples = 0;
					auto& s = g_audio_waves[i];
					full_length = s.wave8k->sample_rate() / 50;
					auto ptr = (uint16_t*)s.wave8k->get_samples(full_length, &mix_samples);
					s.nextSend8k = now + std::chrono::milliseconds((size_t)(mix_samples/(double)s.wave8k->sample_rate()*1000)) - std::chrono::microseconds((size_t)(1e6 / s.wave8k->sample_rate()));			
					if (mix.empty())
					{
						mix = { ptr , ptr + mix_samples };
					}
					else
					{
						std::vector<uint16_t> vec = { ptr , ptr + mix_samples };
						for (size_t j = 0; j < mix.size(); j++)
						{
							if (j >= vec.size())
							{
								break;
							}
							mix[j] = mix_sample(mix[j], vec[j]);
						}
						if (vec.size() > mix.size())
						{
							mix.insert(mix.end(), vec.begin() + mix.size(), vec.end());
						}
					}
					if (s.auto_delete)
					{
						s.wave8k->clear_frames(mix_samples);
					}
					else
					{
						s.flPlayPos8k += (float)mix_samples / s.wave8k->sample_rate();
						s.wave8k->seek(mix_samples, audio_wave::seekCurr);
					}
				}
				speexDataLen = EncodeVoice(sound->second.senderClientIndex, reinterpret_cast<char*>(mix.data()), mix.size(), sound->second.FrameCodec.get(), speexBuf, sizeof(speexBuf));				
			}
			else
			{
				using namespace std::chrono_literals;
				handled_sounds.insert(sound->first);
				int ms = 160;
				full_length = sound->second.wave8k->sample_rate() / 50;
				sample_buffer = sound->second.wave8k->get_samples(full_length, &n_samples);
				sound->second.nextSend8k = now + std::chrono::milliseconds((size_t)(n_samples/(double)sound->second.wave8k->sample_rate()*1000)) - std::chrono::microseconds((size_t)(1e6 / sound->second.wave8k->sample_rate()));
				

				speexDataLen = EncodeVoice(sound->second.senderClientIndex, reinterpret_cast<char*>(sample_buffer), n_samples, sound->second.FrameCodec.get(), speexBuf, sizeof(speexBuf));
				
				if (sound->second.auto_delete)
				{
					sound->second.wave8k->clear_frames(n_samples);
				}
				else
				{
					sound->second.flPlayPos8k += (float)n_samples / sound->second.wave8k->sample_rate();
					sound->second.wave8k->seek(n_samples, audio_wave::seekCurr);
				}
			}

			for (int i = 0; i < maxclients; i++)
			{
				CRevoicePlayer* player = &g_Players[i];
				IGameClient* client = player->GetClient();
				if (!client->IsActive() && !client->IsConnected())
					continue;

				if (g_mute_map[i][sound->second.senderClientIndex] || g_mute_map[i][-1])
					continue;
				
				if (sound->second.receivers.find(-1) == sound->second.receivers.end() && sound->second.receivers.find(i) == sound->second.receivers.end())
				{
					continue;
				}

				if (player->GetCodecType() != vct_speex)
				{
					continue;
				}

				char* sendBuf = speexBuf;
				int nSendLen = speexDataLen;
				if (nSendLen == 0 || sendBuf == nullptr)
					continue;
				sizebuf_t* dstDatagram = client->GetDatagram();
				if (dstDatagram->cursize + nSendLen + 4 < dstDatagram->maxsize) {
					g_RehldsFuncs->MSG_WriteByte(dstDatagram, svc_voicedata);
					g_RehldsFuncs->MSG_WriteByte(dstDatagram, sound->second.senderClientIndex);
					g_RehldsFuncs->MSG_WriteShort(dstDatagram, nSendLen);
					g_RehldsFuncs->MSG_WriteBuf(dstDatagram, nSendLen, sendBuf);
				}
			}
		}

		
	}
	
}

bool Revoice_Main_Init()
{
	g_RehldsHookchains->SV_DropClient()->registerHook(&SV_DropClient_hook, HC_PRIORITY_DEFAULT + 1);
	g_RehldsHookchains->HandleNetCommand()->registerHook(&Rehlds_HandleNetCommand, HC_PRIORITY_DEFAULT + 1);
	g_RehldsHookchains->SV_WriteVoiceCodec()->registerHook(&SV_WriteVoiceCodec_hooked, HC_PRIORITY_DEFAULT + 1);
	return true;
}

void Revoice_Main_DeInit()
{
	g_RehldsHookchains->SV_DropClient()->unregisterHook(&SV_DropClient_hook);
	g_RehldsHookchains->HandleNetCommand()->unregisterHook(&Rehlds_HandleNetCommand);
	g_RehldsHookchains->SV_WriteVoiceCodec()->unregisterHook(&SV_WriteVoiceCodec_hooked);

	Revoice_DeInit_Cvars();
}

// Entity API
void OnClientCommandReceiving(edict_t *pClient) {
	CRevoicePlayer *plr = GetPlayerByEdict(pClient);
	auto command = CMD_ARGV(0);

	if (FStrEq(command, "VTC_CheckStart")) {
		plr->SetCheckingState(1);
		plr->SetCodecType(CodecType::vct_speex);
		RETURN_META(MRES_SUPERCEDE);
	} else if (plr->GetCheckingState()) {
		if (FStrEq(command, "vgui_runscript")) {
			plr->SetCheckingState(2);
			RETURN_META(MRES_SUPERCEDE);
		} else if (FStrEq(command, "VTC_CheckEnd")) {
			plr->SetCodecType(plr->GetCheckingState() == 2 ? vct_opus : vct_speex);
			plr->SetCheckingState(0);	
			RETURN_META(MRES_SUPERCEDE);
		}
	}

	RETURN_META(MRES_IGNORED);
}