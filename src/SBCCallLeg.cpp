#include "SBCCallLeg.h"

#include "SBCCallControlAPI.h"

#include "log.h"
#include "AmUtils.h"
#include "AmAudio.h"
#include "AmPlugIn.h"
#include "AmMediaProcessor.h"
#include "AmConfigReader.h"
#include "AmSessionContainer.h"
#include "AmSipHeaders.h"
#include "SBCSimpleRelay.h"
#include "RegisterDialog.h"
#include "SubscriptionDialog.h"

#include "sip/pcap_logger.h"
#include "sip/sip_parser.h"
#include "sip/sip_trans.h"

#include "HeaderFilter.h"
#include "ParamReplacer.h"
#include "SDPFilter.h"

#include <algorithm>

#include "AmAudioFileRecorder.h"
#include "radius_hooks.h"
#include "Sensors.h"

using namespace std;

#define TRACE DBG

#define FILE_RECORDER_COMPRESSED_EXT ".mp3"
#define FILE_RECORDER_RAW_EXT        ".wav"

inline void replace(string& s, const string& from, const string& to){
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != string::npos) {
		s.replace(pos, from.length(), to);
		pos += s.length();
	}
}

///////////////////////////////////////////////////////////////////////////////////////////

// map stream index and transcoder payload index (two dimensions) into one under
// presumption that there will be less than 128 payloads for transcoding
// (might be handy to remember mapping only for dynamic ones (96-127)
#define MAP_INDEXES(stream_idx, payload_idx) ((stream_idx) * 128 + payload_idx)

void PayloadIdMapping::map(int stream_index, int payload_index, int payload_id)
{
  mapping[MAP_INDEXES(stream_index, payload_index)] = payload_id;
}

int PayloadIdMapping::get(int stream_index, int payload_index)
{
  std::map<int, int>::iterator i = mapping.find(MAP_INDEXES(stream_index, payload_index));
  if (i != mapping.end()) return i->second;
  else return -1;
}

void PayloadIdMapping::reset()
{
  mapping.clear();
}

#undef MAP_INDEXES

///////////////////////////////////////////////////////////////////////////////////////////

// A leg constructor (from SBCDialog)
SBCCallLeg::SBCCallLeg(
	CallCtx *call_ctx,
	AmSipDialog* p_dlg,
	AmSipSubscription* p_subs)
  : CallLeg(p_dlg,p_subs),
    m_state(BB_Init),
    auth(NULL),
    logger(NULL),
	sensor(NULL),
	yeti(Yeti::instance()),
	call_ctx(call_ctx),
	router(yeti.router),
	cdr_list(yeti.cdr_list),
	rctl(yeti.rctl),
	call_profile(*call_ctx->getCurrentProfile()),
	placeholders_hash(call_profile.placeholders_hash)
{
  set_sip_relay_only(false);
  dlg->setRel100State(Am100rel::REL100_IGNORED);

  if(call_profile.rtprelay_bw_limit_rate > 0 &&
     call_profile.rtprelay_bw_limit_peak > 0) {

    RateLimit* limit = new RateLimit(call_profile.rtprelay_bw_limit_rate,
				     call_profile.rtprelay_bw_limit_peak,
				     1000);
    rtp_relay_rate_limit.reset(limit);
  }

  if(call_profile.global_tag.empty()) {
      global_tag = getLocalTag();
  } else {
      global_tag = call_profile.global_tag;
  }

}

// B leg constructor (from SBCCalleeSession)
SBCCallLeg::SBCCallLeg(
	SBCCallLeg* caller,
	AmSipDialog* p_dlg,
	AmSipSubscription* p_subs)
  : auth(NULL),
    call_profile(caller->getCallProfile()),
    placeholders_hash(caller->getPlaceholders()),
    CallLeg(caller,p_dlg,p_subs),
    global_tag(caller->getGlobalTag()),
    logger(NULL),
	sensor(NULL),
	call_ctx(caller->getCallCtx()),
	yeti(Yeti::instance()),
	router(yeti.router),
	cdr_list(yeti.cdr_list),
	rctl(yeti.rctl)
{
  // FIXME: do we want to inherit cc_vars from caller?
  // Can be pretty dangerous when caller stored pointer to object - we should
  // not probably operate on it! But on other hand it could be handy for
  // something, so just take care when using stored objects...
  // call_profile.cc_vars.clear();

  dlg->setRel100State(Am100rel::REL100_IGNORED);

  // we need to apply it here instead of in applyBProfile because we have caller
  // here (FIXME: do it on better place and better way than accessing internals
  // of caller->dlg directly)
  if (call_profile.transparent_dlg_id && caller) {
    dlg->setCallid(caller->dlg->getCallid());
    dlg->setExtLocalTag(caller->dlg->getRemoteTag());
    dlg->cseq = caller->dlg->r_cseq;
  }

  // copy RTP rate limit from caller leg
  if(caller->rtp_relay_rate_limit.get()) {
    rtp_relay_rate_limit.reset(new RateLimit(*caller->rtp_relay_rate_limit.get()));
  }

  init();

  setLogger(caller->getLogger());
}

SBCCallLeg::SBCCallLeg(AmSipDialog* p_dlg, AmSipSubscription* p_subs)
  : CallLeg(p_dlg,p_subs),
    m_state(BB_Init),
    auth(NULL),
	logger(NULL),
    sensor(NULL),
    yeti(Yeti::instance()),
    router(yeti.router),
    cdr_list(yeti.cdr_list),
    rctl(yeti.rctl)
{ }

void SBCCallLeg::init()
{
    call_ctx->inc();

    Cdr *cdr = call_ctx->cdr;

    if(a_leg) {
        ostringstream ss;
        ss << yeti.config.msg_logger_dir << '/' <<
              getLocalTag() << "_" <<
              int2str(yeti.config.node_id) << ".pcap";
        call_profile.set_logger_path(ss.str());

        cdr->update_sbc(call_profile);
        setSensor(Sensors::instance()->getSensor(call_profile.aleg_sensor_id));
        cdr->update_init_aleg(getLocalTag(),
                              global_tag,
                              getCallID());
    } else {
        if(!call_profile.callid.empty()){
            string id = AmSession::getNewId();
            replace(call_profile.callid,"%uuid",id);
        }
        setSensor(Sensors::instance()->getSensor(call_profile.bleg_sensor_id));
        cdr->update_init_bleg(call_profile.callid.empty()? getCallID() : call_profile.callid);
    }

    if(call_profile.record_audio){
        ostringstream ss;
        ss	<< yeti.config.audio_recorder_dir << '/'
            << global_tag << "_"
            << int2str(yeti.config.node_id) <<  "_leg"
            << (a_leg ? "a" : "b")
            << (yeti.config.audio_recorder_compress ?
                FILE_RECORDER_COMPRESSED_EXT :
                FILE_RECORDER_RAW_EXT);
        call_profile.audio_record_path = ss.str();

        AmAudioFileRecorderProcessor::instance()->addRecorder(
            getLocalTag(),
            call_profile.audio_record_path);
        setRecordAudio(true);
    }
}

void SBCCallLeg::onStart()
{
  // this should be the first thing called in session's thread
  CallLeg::onStart();
  if (!a_leg) applyBProfile(); // A leg needs to evaluate profile first
  else if (!getOtherId().empty()) {
    // A leg but we already have a peer, what means that this call leg was
    // created as an A leg for already existing B leg (for example call
    // transfer)
    // we need to apply a profile, we use B profile and understand it as an
    // "outbound" profile though we are in A leg
    applyBProfile();
  }
}

void SBCCallLeg::updateCallProfile(const SBCCallProfile &new_profile)
{
    call_profile = new_profile;
    placeholders_hash.update(call_profile.placeholders_hash);
}

void SBCCallLeg::applyAProfile()
{
  // apply A leg configuration (but most of the configuration is applied in
  // SBCFactory::onInvite)

  setAllow1xxWithoutToTag(call_profile.allow_1xx_without_to_tag);

  if (call_profile.rtprelay_enabled || call_profile.transcoder.isActive()) {
    DBG("Enabling RTP relay mode for SBC call\n");

    setRtpRelayForceSymmetricRtp(call_profile.aleg_force_symmetric_rtp_value);
    DBG("%s\n",getRtpRelayForceSymmetricRtp() ?
	"forcing symmetric RTP (passive mode)":
	"disabled symmetric RTP (normal mode)");
	setRtpEndlessSymmetricRtp(call_profile.bleg_symmetric_rtp_nonstop);
	setRtpSymmetricRtpIgnoreRTCP(call_profile.bleg_symmetric_rtp_ignore_rtcp);

    if (call_profile.aleg_rtprelay_interface_value >= 0) {
      setRtpInterface(call_profile.aleg_rtprelay_interface_value);
      DBG("using RTP interface %i for A leg\n", rtp_interface);
    }

    setRtpRelayTransparentSeqno(call_profile.rtprelay_transparent_seqno);
    setRtpRelayTransparentSSRC(call_profile.rtprelay_transparent_ssrc);
	setRtpRelayTimestampAligning(call_profile.relay_timestamp_aligning);
    setEnableDtmfRtpFiltering(call_profile.rtprelay_dtmf_filtering);
    setEnableDtmfRtpDetection(call_profile.rtprelay_dtmf_detection);
	setEnableDtmfForceRelay(call_profile.rtprelay_force_dtmf_relay);
	setEnableCNForceRelay(call_profile.force_relay_CN);
	setEnableRtpPing(call_profile.aleg_rtp_ping);
	setRtpTimeout(call_profile.dead_rtp_time);
	setIgnoreRelayStreams(call_profile.filter_noaudio_streams);

    if(call_profile.transcoder.isActive()) {
      setRtpRelayMode(RTP_Transcoding);
      switch(call_profile.transcoder.dtmf_mode) {
      case SBCCallProfile::TranscoderSettings::DTMFAlways:
        enable_dtmf_transcoding = true; break;
      case SBCCallProfile::TranscoderSettings::DTMFNever:
        enable_dtmf_transcoding = false; break;
      case SBCCallProfile::TranscoderSettings::DTMFLowFiCodecs:
        enable_dtmf_transcoding = false;
        lowfi_payloads = call_profile.transcoder.lowfi_codecs;
        break;
      };
    }
    else {
      setRtpRelayMode(RTP_Relay);
    }

    // copy stats counters
    rtp_pegs = call_profile.aleg_rtp_counters;
  }

  if(!call_profile.dlg_contact_params.empty())
    dlg->setContactParams(call_profile.dlg_contact_params);
}

int SBCCallLeg::applySSTCfg(AmConfigReader& sst_cfg, 
			   const AmSipRequest* p_req)
{
  DBG("Enabling SIP Session Timers\n");  
  if (NULL == SBCFactory::instance()->session_timer_fact) {
    ERROR("session_timer module not loaded - "
	  "unable to create call with SST\n");
    return -1;
  }
    
  if (p_req && !SBCFactory::instance()->session_timer_fact->
      onInvite(*p_req, sst_cfg)) {
    return -1;
  }

  AmSessionEventHandler* h = SBCFactory::instance()->session_timer_fact->
    getHandler(this);

  if (!h) {
    ERROR("could not get a session timer event handler\n");
    return -1;
  }

  if (h->configure(sst_cfg)) {
    ERROR("Could not configure the session timer: "
	  "disabling session timers.\n");
    delete h;
  }
  else {
    addHandler(h);
    // hack: repeat calling the handler again to start timers because
    // it was called before SST was applied
    if(p_req) h->onSipRequest(*p_req);
  }

  return 0;
}

void SBCCallLeg::applyBProfile()
{
  // TODO: fix this!!! (see d85ed5c7e6b8d4c24e7e5b61c732c2e1ddd31784)
  // if (!call_profile.contact.empty()) {
  //   dlg->contact_uri = SIP_HDR_COLSP(SIP_HDR_CONTACT) + call_profile.contact + CRLF;
  // }

  setAllow1xxWithoutToTag(call_profile.allow_1xx_without_to_tag);

  if (call_profile.auth_enabled) {
    // adding auth handler
    AmSessionEventHandlerFactory* uac_auth_f =
      AmPlugIn::instance()->getFactory4Seh("uac_auth");
    if (NULL == uac_auth_f)  {
      INFO("uac_auth module not loaded. uac auth NOT enabled.\n");
    } else {
      AmSessionEventHandler* h = uac_auth_f->getHandler(this);

      // we cannot use the generic AmSessi(onEvent)Handler hooks,
      // because the hooks don't work in AmB2BSession
      setAuthHandler(h);
      DBG("uac auth enabled for callee session.\n");
    }
  }

  if (call_profile.sst_enabled_value) {
    if(applySSTCfg(call_profile.sst_b_cfg,NULL) < 0) {
       throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }
  }

  if (!call_profile.outbound_proxy.empty()) {
    dlg->outbound_proxy = call_profile.outbound_proxy;
    dlg->force_outbound_proxy = call_profile.force_outbound_proxy;
  }

  if (!call_profile.next_hop.empty()) {
    DBG("set next hop to '%s' (1st_req=%s,fixed=%s)\n",
	call_profile.next_hop.c_str(), call_profile.next_hop_1st_req?"true":"false",
	call_profile.next_hop_fixed?"true":"false");
    dlg->setNextHop(call_profile.next_hop);
    dlg->setNextHop1stReq(call_profile.next_hop_1st_req);
    dlg->setNextHopFixed(call_profile.next_hop_fixed);
  }

  DBG("patch_ruri_next_hop = %i",call_profile.patch_ruri_next_hop);
  dlg->setPatchRURINextHop(call_profile.patch_ruri_next_hop);

  // was read from caller but reading directly from profile now
  if (call_profile.outbound_interface_value >= 0)
    dlg->setOutboundInterface(call_profile.outbound_interface_value);

  // was read from caller but reading directly from profile now
  if (call_profile.rtprelay_enabled || call_profile.transcoder.isActive()) {

    if (call_profile.rtprelay_interface_value >= 0)
      setRtpInterface(call_profile.rtprelay_interface_value);

    setRtpRelayForceSymmetricRtp(call_profile.force_symmetric_rtp_value);
    DBG("%s\n",getRtpRelayForceSymmetricRtp() ?
	"forcing symmetric RTP (passive mode)":
	"disabled symmetric RTP (normal mode)");
	setRtpEndlessSymmetricRtp(call_profile.bleg_symmetric_rtp_nonstop);
	setRtpSymmetricRtpIgnoreRTCP(call_profile.bleg_symmetric_rtp_ignore_rtcp);

    setRtpRelayTransparentSeqno(call_profile.rtprelay_transparent_seqno);
    setRtpRelayTransparentSSRC(call_profile.rtprelay_transparent_ssrc);
	setRtpRelayTimestampAligning(call_profile.relay_timestamp_aligning);
    setEnableDtmfRtpFiltering(call_profile.rtprelay_dtmf_filtering);
    setEnableDtmfRtpDetection(call_profile.rtprelay_dtmf_detection);
	setEnableDtmfForceRelay(call_profile.rtprelay_force_dtmf_relay);
	setEnableCNForceRelay(call_profile.force_relay_CN);
	setEnableRtpPing(call_profile.bleg_rtp_ping);
	setRtpTimeout(call_profile.dead_rtp_time);
	setIgnoreRelayStreams(call_profile.filter_noaudio_streams);

    // copy stats counters
    rtp_pegs = call_profile.bleg_rtp_counters;
  }

  // was read from caller but reading directly from profile now
  if (!call_profile.callid.empty()) 
    dlg->setCallid(call_profile.callid);

  if(!call_profile.bleg_dlg_contact_params.empty())
    dlg->setContactParams(call_profile.bleg_dlg_contact_params);

  setInviteTransactionTimeout(call_profile.inv_transaction_timeout);
  setInviteRetransmitTimeout(call_profile.inv_srv_failover_timeout);
}

int SBCCallLeg::relayEvent(AmEvent* ev)
{
  int res = yeti.relayEvent(this, ev);
  if (res > 0) return 0;
  if (res < 0) return res;


    switch (ev->event_id) {
      case B2BSipRequest:
        {
          B2BSipRequestEvent* req_ev = dynamic_cast<B2BSipRequestEvent*>(ev);
          assert(req_ev);

          inplaceHeaderPatternFilter(
            req_ev->req.hdrs,
            a_leg ? call_profile.headerfilter_a2b : call_profile.headerfilter_b2a
          );

	  if((a_leg && call_profile.keep_vias)
	     || (!a_leg && call_profile.bleg_keep_vias)) {
	    req_ev->req.hdrs = req_ev->req.vias + req_ev->req.hdrs;
	  }
        }
        break;

      case B2BSipReply:
        {
          B2BSipReplyEvent* reply_ev = dynamic_cast<B2BSipReplyEvent*>(ev);
          assert(reply_ev);

          if(call_profile.transparent_dlg_id &&
	     (reply_ev->reply.from_tag == dlg->getExtLocalTag()))
            reply_ev->reply.from_tag = dlg->getLocalTag();
          /*

          if (call_profile.headerfilter.size() ||
              call_profile.reply_translations.size()) {
            // header filter
            if (call_profile.headerfilter.size()) {
              inplaceHeaderFilter(reply_ev->reply.hdrs, call_profile.headerfilter);
            }

            // reply translations
            map<unsigned int, pair<unsigned int, string> >::iterator it =
              call_profile.reply_translations.find(reply_ev->reply.code);

            if (it != call_profile.reply_translations.end()) {
              DBG("translating reply %u %s => %u %s\n",
                  reply_ev->reply.code, reply_ev->reply.reason.c_str(),
                  it->second.first, it->second.second.c_str());
              reply_ev->reply.code = it->second.first;
              reply_ev->reply.reason = it->second.second;
            }
          }*/
        }

        break;
    }

  return CallLeg::relayEvent(ev);
}

SBCCallLeg::~SBCCallLeg()
{
  if (auth)
    delete auth;
  if (logger) dec_ref(logger);
  if(sensor) dec_ref(sensor);
}

void SBCCallLeg::onBeforeDestroy()
{
	DBG("%s(%p|%s,leg%s)",FUNC_NAME,
		this,getLocalTag().c_str(),a_leg?"A":"B");

	CallCtx *ctx = call_ctx;
	if(!ctx) return;

	call_ctx->lock();
	call_ctx = NULL;

	if(call_profile.record_audio) {
		AmAudioFileRecorderProcessor::instance()->removeRecorder(getLocalTag());
	}

	if(ctx->dec_and_test()) {
		DBG("last leg destroy");
		SqlCallProfile *p = ctx->getCurrentProfile();
		if(NULL!=p) rctl.put(p->resource_handler);
		Cdr *cdr = ctx->cdr;
		if(cdr) {
			cdr_list.erase(cdr);
			router.write_cdr(cdr,true);
		}
		ctx->unlock();
		delete ctx;
	} else {
		ctx->unlock();
	}
}

UACAuthCred* SBCCallLeg::getCredentials() {
  if (a_leg) return &call_profile.auth_aleg_credentials;
  else return &call_profile.auth_credentials;
}

void SBCCallLeg::onSipRequest(const AmSipRequest& req) {
  // AmB2BSession does not call AmSession::onSipRequest for 
  // forwarded requests - so lets call event handlers here
  // todo: this is a hack, replace this by calling proper session 
  // event handler in AmB2BSession
  bool fwd = sip_relay_only && (req.method != SIP_METH_CANCEL);
  if (fwd) {
    CALL_EVENT_H(onSipRequest,req);
  }

  if (fwd && call_profile.messagefilter.size()) {
    for (vector<FilterEntry>::iterator it=
	   call_profile.messagefilter.begin(); 
	 it != call_profile.messagefilter.end(); it++) {

      if (isActiveFilter(it->filter_type)) {
	bool is_filtered = (it->filter_type == Whitelist) ^ 
	  (it->filter_list.find(req.method) != it->filter_list.end());
	if (is_filtered) {
	  DBG("replying 405 to filtered message '%s'\n", req.method.c_str());
	  dlg->reply(req, 405, "Method Not Allowed", NULL, "", SIP_FLAGS_VERBATIM);
	  return;
	}
      }
    }
  }

  if(call_ctx->initial_invite) {
    if(yeti.onInDialogRequest(this, req) == StopProcessing) return;
  }

  if (fwd && req.method == SIP_METH_INVITE) {
    DBG("replying 100 Trying to INVITE to be fwd'ed\n");
    dlg->reply(req, 100, SIP_REPLY_TRYING);
  }

  CallLeg::onSipRequest(req);
}

void SBCCallLeg::setOtherId(const AmSipReply& reply)
{
  DBG("setting other_id to '%s'",reply.from_tag.c_str());
  setOtherId(reply.from_tag);
  if(call_profile.transparent_dlg_id && !reply.to_tag.empty()) {
    dlg->setExtLocalTag(reply.to_tag);
  }
}

void SBCCallLeg::onInitialReply(B2BSipReplyEvent *e)
{
  if (call_profile.transparent_dlg_id && !e->reply.to_tag.empty()
      && dlg->getStatus() != AmBasicSipDialog::Connected) {
    dlg->setExtLocalTag(e->reply.to_tag);
  }
  CallLeg::onInitialReply(e);
}

void SBCCallLeg::onSipReply(const AmSipRequest& req, const AmSipReply& reply,
			   AmBasicSipDialog::Status old_dlg_status)
{
  TransMap::iterator t = relayed_req.find(reply.cseq);
  bool fwd = t != relayed_req.end();

  DBG("onSipReply: %i %s (fwd=%i)\n",reply.code,reply.reason.c_str(),fwd);
  DBG("onSipReply: content-type = %s\n",reply.body.getCTStr().c_str());
  if (fwd) {
    CALL_EVENT_H(onSipReply, req, reply, old_dlg_status);
  }

  if (NULL != auth) {
    // only for SIP authenticated
    unsigned int cseq_before = dlg->cseq;
    if (auth->onSipReply(req, reply, old_dlg_status)) {
      if (cseq_before != dlg->cseq) {
        DBG("uac_auth consumed reply with cseq %d and resent with cseq %d; "
            "updating relayed_req map\n", reply.cseq, cseq_before);
        updateUACTransCSeq(reply.cseq, cseq_before);

	// don't relay to other leg, process in AmSession
	AmSession::onSipReply(req, reply, old_dlg_status);
	// skip presenting reply to ext_cc modules, too
	return;
      }
    }
  }

  if(yeti.onInDialogReply(this, reply) == StopProcessing) return;

  CallLeg::onSipReply(req, reply, old_dlg_status);
}

void SBCCallLeg::onSendRequest(AmSipRequest& req, int &flags) {
  yeti.onSendRequest(this, req, flags);
  if(a_leg) {
    if (!call_profile.aleg_append_headers_req.empty()) {

		size_t start_pos = 0;
		while (start_pos<call_profile.aleg_append_headers_req.length()) {
			int res;
			size_t name_end, val_begin, val_end, hdr_end;
			if ((res = skip_header(call_profile.aleg_append_headers_req, start_pos, name_end, val_begin,
					val_end, hdr_end)) != 0) {
				ERROR("skip_header for '%s' pos: %ld, return %d",
						call_profile.aleg_append_headers_req.c_str(),start_pos,res);
				throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
			}
			string hdr_name = call_profile.aleg_append_headers_req.substr(start_pos, name_end-start_pos);
			start_pos = hdr_end;
			while(!getHeader(req.hdrs,hdr_name).empty()){
				removeHeader(req.hdrs,hdr_name);
			}
		}

      DBG("appending '%s' to outbound request (A leg)\n",
	  call_profile.aleg_append_headers_req.c_str());
      req.hdrs+=call_profile.aleg_append_headers_req;
    }
  }
  else {

	  size_t start_pos = 0;
	  while (start_pos<call_profile.append_headers_req.length()) {
		  int res;
		  size_t name_end, val_begin, val_end, hdr_end;
		  if ((res = skip_header(call_profile.append_headers_req, start_pos, name_end, val_begin,
				  val_end, hdr_end)) != 0) {
			  ERROR("skip_header for '%s' pos: %ld, return %d",
					  call_profile.append_headers_req.c_str(),start_pos,res);
			  throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
		  }
		  string hdr_name = call_profile.append_headers_req.substr(start_pos, name_end-start_pos);
		  start_pos = hdr_end;
		  while(!getHeader(req.hdrs,hdr_name).empty()){
			  removeHeader(req.hdrs,hdr_name);
		  }
	  }

    if (!call_profile.append_headers_req.empty()) {
      DBG("appending '%s' to outbound request (B leg)\n", 
	  call_profile.append_headers_req.c_str());
      req.hdrs+=call_profile.append_headers_req;
    }
  }

  if (NULL != auth) {
    DBG("auth->onSendRequest cseq = %d\n", req.cseq);
    auth->onSendRequest(req, flags);
  }

  CallLeg::onSendRequest(req, flags);
}

void SBCCallLeg::onRemoteDisappeared(const AmSipReply& reply)
{
  if (yeti.onRemoteDisappeared(this, reply) == StopProcessing) return;
  CallLeg::onRemoteDisappeared(reply);
}

void SBCCallLeg::onBye(const AmSipRequest& req)
{
  if (yeti.onBye(this, req) == StopProcessing) return;
  CallLeg::onBye(req);
}

void SBCCallLeg::onOtherBye(const AmSipRequest& req)
{
  if (yeti.onOtherBye(this, req) == StopProcessing) return;
  CallLeg::onOtherBye(req);
}

void SBCCallLeg::onDtmf(AmDtmfEvent* e)
{
  DBG("received DTMF on %c-leg (%i;%i)\n", a_leg ? 'A': 'B', e->event(), e->duration());

  if (yeti.onDtmf(this, e)  == StopProcessing) return;

  AmB2BMedia *ms = getMediaSession();
  if(ms) {
	DBG("sending DTMF (%i;%i)\n", e->event(), e->duration());
	ms->sendDtmf(!a_leg,e->event(),e->duration());
  }
}

void SBCCallLeg::updateLocalSdp(AmSdp &sdp)
{
  // anonymize SDP if configured to do so (we need to have our local media IP,
  // not the media IP of our peer leg there)
  if (call_profile.anonymize_sdp) normalizeSDP(sdp, call_profile.anonymize_sdp, advertisedIP());

  // remember transcodable payload IDs
  //if (call_profile.transcoder.isActive()) savePayloadIDs(sdp);
  CallLeg::updateLocalSdp(sdp);
}

void SBCCallLeg::onControlCmd(string& cmd, AmArg& params) {
  if (cmd == "teardown") {
    if (a_leg) {
      // was for caller:
      DBG("teardown requested from control cmd\n");
      stopCall("ctrl-cmd");
      // FIXME: don't we want to relay the controll event as well?
    }
    else {
      // was for callee:
      DBG("relaying teardown control cmd to A leg\n");
      relayEvent(new SBCControlEvent(cmd, params));
      // FIXME: don't we want to stopCall as well?
    }
    return;
  }
  DBG("ignoring unknown control cmd : '%s'\n", cmd.c_str());
}


void SBCCallLeg::process(AmEvent* ev) {
  if (yeti.onEvent(this, ev) == StopProcessing) return;

  if (a_leg) {
    // was for caller (SBCDialog):
    AmPluginEvent* plugin_event = dynamic_cast<AmPluginEvent*>(ev);
    if(plugin_event && plugin_event->name == "timer_timeout") {
      int timer_id = plugin_event->data.get(0).asInt();
      if (timer_id >= SBC_TIMER_ID_CALL_TIMERS_START &&
          timer_id <= SBC_TIMER_ID_CALL_TIMERS_END) {
        DBG("timer %d timeout, stopping call\n", timer_id);
        stopCall("timer");
        ev->processed = true;
      }
    }

    SBCCallTimerEvent* ct_event;
    if (ev->event_id == SBCCallTimerEvent_ID &&
        (ct_event = dynamic_cast<SBCCallTimerEvent*>(ev)) != NULL) {
      switch (m_state) {
        case BB_Connected: 
          switch (ct_event->timer_action) {
            case SBCCallTimerEvent::Remove:
              DBG("removing timer %d on call timer request\n", ct_event->timer_id);
              removeTimer(ct_event->timer_id); return;
            case SBCCallTimerEvent::Set:
              DBG("setting timer %d to %f on call timer request\n",
                  ct_event->timer_id, ct_event->timeout);
              setTimer(ct_event->timer_id, ct_event->timeout); return;
            case SBCCallTimerEvent::Reset:
              DBG("resetting timer %d to %f on call timer request\n",
                  ct_event->timer_id, ct_event->timeout);
              removeTimer(ct_event->timer_id);
              setTimer(ct_event->timer_id, ct_event->timeout);
              return;
            default: ERROR("unknown timer_action in sbc call timer event\n"); return;
          }

        case BB_Init:
        case BB_Dialing:

          switch (ct_event->timer_action) {
            case SBCCallTimerEvent::Remove: 
              clearCallTimer(ct_event->timer_id); 
              return;

            case SBCCallTimerEvent::Set:
            case SBCCallTimerEvent::Reset:
              saveCallTimer(ct_event->timer_id, ct_event->timeout); 
              return;

            default: ERROR("unknown timer_action in sbc call timer event\n"); return;
          }
          break;

        default: break;
      }
    }
  }

  SBCControlEvent* ctl_event;
  if (ev->event_id == SBCControlEvent_ID &&
      (ctl_event = dynamic_cast<SBCControlEvent*>(ev)) != NULL) {
    onControlCmd(ctl_event->cmd, ctl_event->params);
    return;
  }

  CallLeg::process(ev);
}


//////////////////////////////////////////////////////////////////////////////////////////
// was for caller only (SBCDialog)
// FIXME: move the stuff related to CC interface outside of this class?


#define REPLACE_VALS req, app_param, ruri_parser, from_parser, to_parser

void SBCCallLeg::onInvite(const AmSipRequest& req){
	DBG("processing initial INVITE %s\n", req.r_uri.c_str());

	ctx.call_profile = &call_profile;
	ctx.app_param = getHeader(req.hdrs, PARAM_HDR, true);

	init();

	modified_req = req;
	aleg_modified_req = req;
	uac_req = req;

	if (!logger &&
		!call_profile.get_logger_path().empty() &&
		(call_profile.log_sip || call_profile.log_rtp))
	{
	  // open the logger if not already opened
	  ParamReplacerCtx ctx(&call_profile);
	  string log_path = ctx.replaceParameters(call_profile.get_logger_path(),
						  "msg_logger_path",req);
	  if(!openLogger(log_path)){
		  WARN("can't open msg_logger_path: '%s'",log_path.c_str());
	  }
	}

	req.log(call_profile.log_sip?getLogger():NULL,
			call_profile.aleg_sensor_level_id&LOG_SIP_MASK?getSensor():NULL);

	uac_ruri.uri = uac_req.r_uri;
	if(!uac_ruri.parse_uri()) {
	  DBG("Error parsing R-URI '%s'\n",uac_ruri.uri.c_str());
	  throw AmSession::Exception(400,"Failed to parse R-URI");
	}

	call_ctx->cdr->update(req);
	call_ctx->initial_invite = new AmSipRequest(aleg_modified_req);

	if(yeti.config.early_100_trying){
		msg_logger *logger = getLogger();
		if(logger){
			call_ctx->early_trying_logger->relog(logger);
		}
	} else {
		dlg->reply(req,100,"Connecting");
	}

	if(!radius_auth(this,*call_ctx->cdr,call_profile,req)){
		yeti.onRoutingReady(this,aleg_modified_req,modified_req);
	}
}

void SBCCallLeg::onRoutingReady()
{
  call_profile.sst_aleg_enabled =
    ctx.replaceParameters(call_profile.sst_aleg_enabled,
			  "enable_aleg_session_timer", aleg_modified_req);

  call_profile.sst_enabled = ctx.replaceParameters(call_profile.sst_enabled, 
						   "enable_session_timer", aleg_modified_req);

  if ((call_profile.sst_aleg_enabled == "yes") ||
      (call_profile.sst_enabled == "yes")) {

	call_profile.eval_sst_config(ctx,aleg_modified_req,call_profile.sst_a_cfg);
	if(applySSTCfg(call_profile.sst_a_cfg,&aleg_modified_req) < 0) {
      throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }
  }

  if (!call_profile.evaluate(ctx, aleg_modified_req)) {
    ERROR("call profile evaluation failed\n");
    throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
  }

  AmUriParser uac_ruri;
  uac_ruri.uri = uac_req.r_uri;
  if(!uac_ruri.parse_uri()) {
	DBG("Error parsing R-URI '%s'\n",uac_ruri.uri.c_str());
	throw AmSession::Exception(400,"Failed to parse R-URI");
  }

  if(call_profile.contact_hiding) { 
	if(RegisterDialog::decodeUsername(aleg_modified_req.user,uac_ruri)) {
      uac_req.r_uri = uac_ruri.uri_str();
    }
  }
  else if(call_profile.reg_caching) {
    // REG-Cache lookup
	uac_req.r_uri = call_profile.retarget(aleg_modified_req.user,*dlg);
  }

  ruri = call_profile.ruri.empty() ? uac_req.r_uri : call_profile.ruri;
  if(!call_profile.ruri_host.empty()){
    ctx.ruri_parser.uri = ruri;
    if(!ctx.ruri_parser.parse_uri()) {
      WARN("Error parsing R-URI '%s'\n", ruri.c_str());
    }
    else {
      ctx.ruri_parser.uri_port.clear();
      ctx.ruri_parser.uri_host = call_profile.ruri_host;
      ruri = ctx.ruri_parser.uri_str();
    }
  }
  from = call_profile.from.empty() ? aleg_modified_req.from : call_profile.from;
  to = call_profile.to.empty() ? aleg_modified_req.to : call_profile.to;

  applyAProfile();
  call_profile.apply_a_routing(ctx,aleg_modified_req,*dlg);

  m_state = BB_Dialing;

  // prepare request to relay to the B leg(s)

  if(a_leg && call_profile.keep_vias)
	modified_req.hdrs = modified_req.vias + modified_req.hdrs;
  
  est_invite_cseq = uac_req.cseq;

  removeHeader(modified_req.hdrs,PARAM_HDR);
  removeHeader(modified_req.hdrs,"P-App-Name");

  if (call_profile.sst_enabled_value) {
	removeHeader(modified_req.hdrs,SIP_HDR_SESSION_EXPIRES);
	removeHeader(modified_req.hdrs,SIP_HDR_MIN_SE);
  }

	size_t start_pos = 0;
	while (start_pos<call_profile.append_headers.length()) {
		int res;
		size_t name_end, val_begin, val_end, hdr_end;
		if ((res = skip_header(call_profile.append_headers, start_pos, name_end, val_begin,
				val_end, hdr_end)) != 0) {
			ERROR("skip_header for '%s' pos: %ld, return %d",
					call_profile.append_headers.c_str(),start_pos,res);
			throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
		}
		string hdr_name = call_profile.append_headers.substr(start_pos, name_end-start_pos);
		while(!getHeader(modified_req.hdrs,hdr_name).empty()){
			removeHeader(modified_req.hdrs,hdr_name);
		}
		start_pos = hdr_end;
	}

  inplaceHeaderPatternFilter(modified_req.hdrs, call_profile.headerfilter_a2b);

  if (call_profile.append_headers.size() > 2) {
	string append_headers = call_profile.append_headers;
	assertEndCRLF(append_headers);
	modified_req.hdrs+=append_headers;
  }

  /*int res = filterSdp(invite_req.body, invite_req.method);
  if (res < 0) {
    // FIXME: quick hack, throw the exception from the filtering function for
    // requests
    throw AmSession::Exception(488, SIP_REPLY_NOT_ACCEPTABLE_HERE);
  }*/

#undef REPLACE_VALS

  DBG("SBC: connecting to '%s'\n",ruri.c_str());
  DBG("     From:  '%s'\n",from.c_str());
  DBG("     To:  '%s'\n",to.c_str());

  // we evaluated the settings, now we can initialize internals (like RTP relay)
  // we have to use original request (not the altered one) because for example
  // codecs filtered out might be used in direction to caller
  CallLeg::onInvite(aleg_modified_req);

  if (getCallStatus() == Disconnected) {
    // no CC module connected a callee yet
	connectCallee(to, ruri, from, aleg_modified_req, modified_req); // connect to the B leg(s) using modified request
  }
}

void SBCCallLeg::onInviteException(int code,string reason,bool no_reply){
    yeti.onInviteException(this,code,reason,no_reply);
}

void SBCCallLeg::onEarlyEventException(unsigned int code,const string &reason)
{
	setStopped();
	onInviteException(code,reason,false);
	if(code < 300){
		ERROR("%i is not final code. replace it with 500",code);
		code = 500;
	}
	dlg->reply(uac_req,code,reason);
}

void SBCCallLeg::connectCallee(const string& remote_party, 
			       const string& remote_uri,
			       const string &from, 
			       const AmSipRequest &original_invite, 
			       const AmSipRequest &invite)
{
  // FIXME: no fork for now

  SBCCallLeg* callee_session = SBCFactory::instance()->
    getCallLegCreator()->create(this);

  callee_session->setLocalParty(from, from);
  callee_session->setRemoteParty(remote_party, remote_uri);

  DBG("Created B2BUA callee leg, From: %s\n", from.c_str());

  // FIXME: inconsistent with other filtering stuff - here goes the INVITE
  // already filtered so need not to be catched (can not) in relayEvent because
  // it is sent other way
  addCallee(callee_session, invite);
  
  // we could start in SIP relay mode from the beginning if only one B leg, but
  // serial fork might mess it
  // set_sip_relay_only(true);
}

void SBCCallLeg::onCallConnected(const AmSipReply& reply) {
  yeti.onCallConnected(this, reply);
  if (a_leg) { // FIXME: really?
    m_state = BB_Connected;

    if (!startCallTimers())
      return;
  }
}

void SBCCallLeg::onStop() {
  if (a_leg && m_state == BB_Connected) { // m_state might be valid for A leg only
    stopCallTimers();
  }

  m_state = BB_Teardown;

  yeti.onCallEnded(this);
}

void SBCCallLeg::saveCallTimer(int timer, double timeout) {
  call_timers[timer] = timeout;
}

void SBCCallLeg::clearCallTimer(int timer) {
  call_timers.erase(timer);
}

void SBCCallLeg::clearCallTimers() {
  call_timers.clear();
}

/** @return whether successful */
bool SBCCallLeg::startCallTimers() {
  for (map<int, double>::iterator it=
	 call_timers.begin(); it != call_timers.end(); it++) {
    DBG("SBC: starting call timer %i of %f seconds\n", it->first, it->second);
    setTimer(it->first, it->second);
  }

  return true;
}

void SBCCallLeg::stopCallTimers() {
  for (map<int, double>::iterator it=
	 call_timers.begin(); it != call_timers.end(); it++) {
    DBG("SBC: removing call timer %i\n", it->first);
    removeTimer(it->first);
  }
}

void SBCCallLeg::onCallStatusChange(const StatusChangeCause &cause)
{
  yeti.onStateChange(this, cause);
}

void SBCCallLeg::onBLegRefused(AmSipReply& reply)
{
  if (yeti.onBLegRefused(this, reply) == StopProcessing) return;
}

void SBCCallLeg::onCallFailed(CallFailureReason reason, const AmSipReply *reply)
{
  /*switch (reason) {
    case CallRefused:
      if (reply) logCallStart(*reply);
      break;

    case CallCanceled:
      logCanceledCall();
      break;
  }*/
}

bool SBCCallLeg::onBeforeRTPRelay(AmRtpPacket* p, sockaddr_storage* remote_addr)
{
  if(rtp_relay_rate_limit.get() &&
     rtp_relay_rate_limit->limit(p->getBufferSize()))
    return false; // drop

  return true; // relay
}

void SBCCallLeg::onAfterRTPRelay(AmRtpPacket* p, sockaddr_storage* remote_addr)
{
  for(list<atomic_int*>::iterator it = rtp_pegs.begin();
      it != rtp_pegs.end(); ++it) {
    (*it)->inc(p->getBufferSize());
  }
}

void SBCCallLeg::onRTPStreamDestroy(AmRtpStream *stream){
    yeti.onRTPStreamDestroy(this, stream);
}

//////////////////////////////////////////////////////////////////////////////////////////
// body filtering

/*
int SBCCallLeg::filterSdp(AmMimeBody &body, const string &method)
{
  DBG("filtering body\n");

  AmMimeBody* sdp_body = body.hasContentType(SIP_APPLICATION_SDP);
  if (!sdp_body) return 0;

  // filter body for given methods only
  if (!(method == SIP_METH_INVITE ||
       method == SIP_METH_UPDATE ||
       method == SIP_METH_PRACK ||
       method == SIP_METH_ACK)) return 0;

  AmSdp sdp;
  int res = sdp.parse((const char *)sdp_body->getPayload());
  if (0 != res) {
    DBG("SDP parsing failed during body filtering!\n");
    return res;
  }

  bool changed = false;
  bool prefer_existing_codecs = call_profile.codec_prefs.preferExistingCodecs(a_leg);

  bool needs_normalization =
          call_profile.codec_prefs.shouldOrderPayloads(a_leg) ||
          call_profile.transcoder.isActive() ||
          !call_profile.sdpfilter.empty();

  if (needs_normalization) {
    normalizeSDP(sdp, false, ""); // anonymization is done in the other leg to use correct IP address
    changed = true;
  }

  if (!call_profile.mediafilter.empty()) {
    res = filterMedia(sdp, call_profile.mediafilter);
    if (res < 0) {
      // result may be ignored, we need to set the SDP
      string n_body;
      sdp.print(n_body);
      sdp_body->setPayload((const unsigned char*)n_body.c_str(), n_body.length());
      return res;
    }
    changed = true;
  }

  if (prefer_existing_codecs) {
    // We have to order payloads before adding transcoder codecs to leave
    // transcoding as the last chance (existing codecs are preferred thus
    // relaying will be used if possible).
    if (call_profile.codec_prefs.shouldOrderPayloads(a_leg)) {
      call_profile.codec_prefs.orderSDP(sdp, a_leg);
      changed = true;
    }
  }

  // Add transcoder codecs before filtering because otherwise SDP filter could
  // inactivate some media lines which shouldn't be inactivated.

  if (call_profile.transcoder.isActive()) {
    appendTranscoderCodecs(sdp);
    changed = true;
  }

  if (!prefer_existing_codecs) {
    // existing codecs are not preferred - reorder AFTER adding transcoder
    // codecs so it might happen that transcoding will be forced though relaying
    // would be possible
    if (call_profile.codec_prefs.shouldOrderPayloads(a_leg)) {
      call_profile.codec_prefs.orderSDP(sdp, a_leg);
      changed = true;
    }
  }

  // It doesn't make sense to filter out codecs allowed for transcoding and thus
  // if the filter filters them out it can be considered as configuration
  // problem, right?
  // => So we wouldn't try to avoid filtering out transcoder codecs what would
  // just complicate things.

  if (call_profile.sdpfilter.size()) {
    res = filterSDP(sdp, call_profile.sdpfilter);
    changed = true;
  }
  if (call_profile.sdpalinesfilter.size()) {
    // filter SDP "a=lines"
    filterSDPalines(sdp, call_profile.sdpalinesfilter);
    changed = true;
  }

  if (changed) {
    string n_body;
    sdp.print(n_body);
    sdp_body->setPayload((const unsigned char*)n_body.c_str(), n_body.length());
  }

  return res;
}
*/

/*
void SBCCallLeg::appendTranscoderCodecs(AmSdp &sdp)
{
  // append codecs for transcoding, remember the added ones to be able to filter
  // them out from relayed reply!

  // important: normalized SDP should get here

  TRACE("going to append transcoder codecs into SDP\n");
  const std::vector<SdpPayload> &transcoder_codecs = call_profile.transcoder.audio_codecs;

  unsigned stream_idx = 0;
  vector<SdpPayload>::const_iterator p;
  for (vector<SdpMedia>::iterator m = sdp.media.begin(); m != sdp.media.end(); ++m) {

    // handle audio transcoder codecs
    if (m->type == MT_AUDIO) {
      // transcoder codecs can be added only if there are common payloads with
      // the remote (only those allowed for transcoder)
      // if there are no such common payloads adding transcoder codecs can't help
      // because we won't be able to transcode later on!
      // (we have to check for each media stream independently)

      // find first unused dynamic payload number & detect transcodable codecs
      // in original SDP
      int id = 96;
      bool transcodable = false;
      PayloadMask used_payloads;
      for (p = m->payloads.begin(); p != m->payloads.end(); ++p) {
        if (p->payload_type >= id) id = p->payload_type + 1;
        if (containsPayload(transcoder_codecs, *p, m->transport)) transcodable = true;
        used_payloads.set(p->payload_type);
      }

      if (transcodable) {
        // there are some transcodable codecs present in the SDP, we can safely
        // add the other transcoder codecs to the SDP
        unsigned idx = 0;
        for (p = transcoder_codecs.begin(); p != transcoder_codecs.end(); ++p, ++idx) {
          // add all payloads which are not already there
          if (!containsPayload(m->payloads, *p, m->transport)) {
            m->payloads.push_back(*p);
            int &pid = m->payloads.back().payload_type;
            if (pid < 0) {
              // try to use remembered ID
              pid = transcoder_payload_mapping.get(stream_idx, idx);
            }

            if ((pid < 0) || used_payloads.get(pid)) {
              // payload ID is not set or is already used in current SDP, we
              // need to assign a new one
              pid = id++;
            }
          }
        }
        if (id > 128) ERROR("assigned too high payload type number (%d), see RFC 3551\n", id);
      }
      else {
        // no compatible codecs found
        TRACE("can not transcode stream %d - no compatible codecs with transcoder_codecs found\n", stream_idx + 1);
      }

      stream_idx++; // count chosen media type only
    }
  }

  // remembered payload IDs should be used just once, in SDP answer
  // unfortunatelly the SDP answer might be present in 1xx and in 2xx as well so
  // we can't clear it here
  // on other hand it might be useful to use the same payload ID if offer/answer
  // is repeated in the other direction next time
}
*/

/*
void SBCCallLeg::savePayloadIDs(AmSdp &sdp)
{
  unsigned stream_idx = 0;
  std::vector<SdpPayload> &transcoder_codecs = call_profile.transcoder.audio_codecs;
  for (vector<SdpMedia>::iterator m = sdp.media.begin(); m != sdp.media.end(); ++m) {
    if (m->type != MT_AUDIO) continue;

    unsigned idx = 0;
    for (vector<SdpPayload>::iterator p = transcoder_codecs.begin();
        p != transcoder_codecs.end(); ++p, ++idx)
    {
      if (p->payload_type < 0) {
        const SdpPayload *pp = findPayload(m->payloads, *p, m->transport);
        if (pp && (pp->payload_type >= 0))
          transcoder_payload_mapping.map(stream_idx, idx, pp->payload_type);
      }
    }

    stream_idx++; // count chosen media type only
  }
}
*/

bool SBCCallLeg::reinvite(const AmSdp &sdp, unsigned &request_cseq)
{
  request_cseq = 0;

  AmMimeBody body;
  AmMimeBody *sdp_body = body.addPart(SIP_APPLICATION_SDP);
  if (!sdp_body) return false;

  string body_str;
  sdp.print(body_str);
  sdp_body->parse(SIP_APPLICATION_SDP, (const unsigned char*)body_str.c_str(), body_str.length());

  if (dlg->reinvite("", &body, SIP_FLAGS_VERBATIM) != 0) return false;
  request_cseq = dlg->cseq - 1;
  return true;
}

#define CALL_EXT_CC_MODULES(method) \
    yeti.method(this);

void SBCCallLeg::holdRequested()
{
  TRACE("%s: hold requested\n", getLocalTag().c_str());
  CALL_EXT_CC_MODULES(holdRequested);
  CallLeg::holdRequested();
}

void SBCCallLeg::holdAccepted()
{
  TRACE("%s: hold accepted\n", getLocalTag().c_str());
  CALL_EXT_CC_MODULES(holdAccepted);
  CallLeg::holdAccepted();
}

void SBCCallLeg::holdRejected()
{
  TRACE("%s: hold rejected\n", getLocalTag().c_str());
  CALL_EXT_CC_MODULES(holdRejected);
  CallLeg::holdRejected();
}

void SBCCallLeg::resumeRequested()
{
  TRACE("%s: resume requested\n", getLocalTag().c_str());
  CALL_EXT_CC_MODULES(resumeRequested);
  CallLeg::resumeRequested();
}

void SBCCallLeg::resumeAccepted()
{
  TRACE("%s: resume accepted\n", getLocalTag().c_str());
  CALL_EXT_CC_MODULES(resumeAccepted);
  CallLeg::resumeAccepted();
}

void SBCCallLeg::resumeRejected()
{
  TRACE("%s: resume rejected\n", getLocalTag().c_str());
  CALL_EXT_CC_MODULES(resumeRejected);
  CallLeg::resumeRejected();
}

static void replace_address(SdpConnection &c, const string &ip)
{
  if (!c.address.empty()) {
    if (c.addrType == AT_V4) {
      c.address = ip;
      return;
    }
    // TODO: IPv6?
    DBG("unsupported address type for replacing IP");
  }
}

static void alterHoldRequest(AmSdp &sdp, SBCCallProfile::HoldSettings::Activity a, const string &ip)
{
  if (!ip.empty()) replace_address(sdp.conn, ip);
  for (vector<SdpMedia>::iterator m = sdp.media.begin(); m != sdp.media.end(); ++m) {
    if (!ip.empty()) replace_address(m->conn, ip);
    m->recv = (a == SBCCallProfile::HoldSettings::sendrecv || a == SBCCallProfile::HoldSettings::recvonly);
    m->send = (a == SBCCallProfile::HoldSettings::sendrecv || a == SBCCallProfile::HoldSettings::sendonly);
  }
}

void SBCCallLeg::alterHoldRequestImpl(AmSdp &sdp)
{
  if (call_profile.hold_settings.mark_zero_connection(a_leg)) {
    static const string zero("0.0.0.0");
    ::alterHoldRequest(sdp, call_profile.hold_settings.activity(a_leg), zero);
  }
  else {
    if (getRtpRelayMode() == RTP_Direct) {
      // we can not put our IP there if not relaying, using empty not to
      // overwrite existing addresses
      static const string empty;
      ::alterHoldRequest(sdp, call_profile.hold_settings.activity(a_leg), empty);
    }
    else {
      // use public IP to be put into connection addresses (overwrite 0.0.0.0
      // there)
      ::alterHoldRequest(sdp, call_profile.hold_settings.activity(a_leg), advertisedIP());
    }
  }
}

void SBCCallLeg::alterHoldRequest(AmSdp &sdp)
{
  TRACE("altering B2B hold request(%s, %s, %s)\n",
      call_profile.hold_settings.alter_b2b(a_leg) ? "alter B2B" : "do not alter B2B",
      call_profile.hold_settings.mark_zero_connection(a_leg) ? "0.0.0.0" : "own IP",
      call_profile.hold_settings.activity_str(a_leg).c_str()
      );

  if (!call_profile.hold_settings.alter_b2b(a_leg)) return;

  alterHoldRequestImpl(sdp);
}

void SBCCallLeg::processLocalRequest(AmSipRequest &req) {
	DBG("%s() local_tag = %s",FUNC_NAME,getLocalTag().c_str());
	updateLocalBody(req.body);
	dlg->reply(req,200,"OK",&req.body,"",SIP_FLAGS_VERBATIM);
}

void SBCCallLeg::createHoldRequest(AmSdp &sdp)
{
  // hack: we need to have other side SDP (if the stream is hold already
  // it should be marked as inactive)
  // FIXME: fix SDP versioning! (remember generated versions and increase the
  // version number in every SDP passing through?)

  AmMimeBody *s = established_body.hasContentType(SIP_APPLICATION_SDP);
  if (s) sdp.parse((const char*)s->getPayload());
  if (sdp.media.empty()) {
    // established SDP is not valid! generate complete fake
    sdp.version = 0;
    sdp.origin.user = "sems";
    sdp.sessionName = "sems";
    sdp.conn.network = NT_IN;
    sdp.conn.addrType = AT_V4;
    sdp.conn.address = "0.0.0.0";

    sdp.media.push_back(SdpMedia());
    SdpMedia &m = sdp.media.back();
    m.type = MT_AUDIO;
    m.transport = TP_RTPAVP;
    m.send = false;
    m.recv = false;
    m.payloads.push_back(SdpPayload(0));
  }

  AmB2BMedia *ms = getMediaSession();
  if (ms) ms->replaceOffer(sdp, a_leg);

  alterHoldRequestImpl(sdp);
}

void SBCCallLeg::setMediaSession(AmB2BMedia *new_session)
{
	if (new_session) {
		if (call_profile.log_rtp) new_session->setRtpLogger(logger);
		else new_session->setRtpLogger(NULL);

		if(a_leg) {
			if(call_profile.aleg_sensor_level_id&LOG_RTP_MASK)
			new_session->setRtpASensor(sensor);
			else new_session->setRtpASensor(NULL);
		} else {
			if(call_profile.bleg_sensor_level_id&LOG_RTP_MASK)
			new_session->setRtpBSensor(sensor);
			else new_session->setRtpBSensor(NULL);
		}
  }
  CallLeg::setMediaSession(new_session);
}

bool SBCCallLeg::openLogger(const std::string &path)
{
  file_msg_logger *log = new pcap_logger();

  if(log->open(path.c_str()) != 0) {
    // open error
    delete log;
    return false;
  }

  // opened successfully
  setLogger(log);
  return true;
}

void SBCCallLeg::setLogger(msg_logger *_logger)
{
  if (logger) dec_ref(logger); // release the old one

  logger = _logger;
  if (logger) inc_ref(logger);
  if (call_profile.log_sip) dlg->setMsgLogger(logger);
  else dlg->setMsgLogger(NULL);

  AmB2BMedia *m = getMediaSession();
  if (m) {
    if (call_profile.log_rtp) m->setRtpLogger(logger);
    else m->setRtpLogger(NULL);
  }
}

void SBCCallLeg::setSensor(msg_sensor *_sensor){
	DBG("SBCCallLeg[%p]: %cleg. change sensor to %p",this,a_leg?'A':'B',_sensor);
	if (sensor) dec_ref(sensor);
	sensor = _sensor;
	if (sensor) inc_ref(sensor);

	if((a_leg && (call_profile.aleg_sensor_level_id&LOG_SIP_MASK)) ||
		(!a_leg && (call_profile.bleg_sensor_level_id&LOG_SIP_MASK)))
	dlg->setMsgSensor(sensor);
	else dlg->setMsgSensor(NULL);

	AmB2BMedia *m = getMediaSession();
	if(m) {
		if(a_leg) {
			if(call_profile.aleg_sensor_level_id&LOG_RTP_MASK)
			m->setRtpASensor(sensor);
			else m->setRtpASensor(NULL);
		} else {
			if(call_profile.bleg_sensor_level_id&LOG_RTP_MASK)
			m->setRtpBSensor(sensor);
			else m->setRtpBSensor(NULL);
		}
	} else DBG("SBCCallLeg: no media session");
}

void SBCCallLeg::computeRelayMask(const SdpMedia &m, bool &enable, PayloadMask &mask)
{
  if (call_profile.transcoder.isActive()) {
    TRACE("entering transcoder's computeRelayMask(%s)\n", a_leg ? "A leg" : "B leg");

	//SBCCallProfile::TranscoderSettings &transcoder_settings = call_profile.transcoder;
	PayloadMask m1/*, m2*/;
	//bool use_m1 = false;

    /* if "m" contains only "norelay" codecs, relay is enabled for them (main idea
     * of these codecs is to limit network bandwidth and it makes not much sense
     * to transcode between codecs 'which are better to avoid', right?)
     *
     * if "m" contains other codecs, relay is enabled as well
     *
     * => if m contains at least some codecs, relay is enabled */
    enable = !m.payloads.empty();

	/*vector<SdpPayload> &norelay_payloads =
	  a_leg ? transcoder_settings.audio_codecs_norelay_aleg : transcoder_settings.audio_codecs_norelay;*/

    vector<SdpPayload>::const_iterator p;
    for (p = m.payloads.begin(); p != m.payloads.end(); ++p) {

      // do not mark telephone-event payload for relay (and do not use it for
      // transcoding as well)
      if(strcasecmp("telephone-event",p->encoding_name.c_str()) == 0) continue;

      // mark every codec for relay in m2
	  TRACE("marking payload %d for relay\n", p->payload_type);
	  m1.set(p->payload_type);

	  /*if (!containsPayload(norelay_payloads, *p, m.transport)) {
        // this payload can be relayed
        TRACE("m1: marking payload %d for relay\n", p->payload_type);
        m1.set(p->payload_type);

        if (!use_m1 && containsPayload(transcoder_settings.audio_codecs, *p, m.transport)) {
          // the remote SDP contains transcodable codec which can be relayed (i.e.
          // the one with higher "priority" so we want to disable relaying of the
          // payloads which should not be ralyed if possible)
          use_m1 = true;
        }
	  }*/
    }

	/*TRACE("using %s\n", use_m1 ? "m1" : "m2");
	if (use_m1) mask = m1;
	else mask = m2;*/
	if(call_profile.force_relay_CN){
		mask.set(COMFORT_NOISE_PAYLOAD_TYPE);
		TRACE("m1: marking payload 13 (CN) for relay\n");
	}

	mask = m1;
  }
  else {
    // for non-transcoding modes use default
    CallLeg::computeRelayMask(m, enable, mask);
  }
}

int SBCCallLeg::onSdpCompleted(const AmSdp& local, const AmSdp& remote){
    AmSdp offer(local),answer(remote);
    //give extended interfaces chance to transform sdp before relay mask will be computed
    yeti.onSdpCompleted(this, offer, answer);
    return CallLeg::onSdpCompleted(offer, answer);
}

bool SBCCallLeg::getSdpOffer(AmSdp& offer){
	if(yeti.getSdpOffer(this,offer)) return true;
	return CallLeg::getSdpOffer(offer);
}

void SBCCallLeg::b2bInitial1xx(AmSipReply& reply, bool forward)
{
	yeti.onB2Binitial1xx(this,reply,forward);
	return CallLeg::b2bInitial1xx(reply,forward);
}
