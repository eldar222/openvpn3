//
//  cliopthelper.hpp
//  OpenVPN
//
//  Copyright (c) 2012 OpenVPN Technologies, Inc. All rights reserved.
//

// A preliminary parser for OpenVPN client configuration files.

#ifndef OPENVPN_CLIENT_CLIOPTHELPER_H
#define OPENVPN_CLIENT_CLIOPTHELPER_H

#include <vector>
#include <string>
#include <sstream>

#include <openvpn/common/types.hpp>
#include <openvpn/common/exception.hpp>
#include <openvpn/common/options.hpp>
#include <openvpn/common/string.hpp>
#include <openvpn/common/split.hpp>
#include <openvpn/common/splitlines.hpp>
#include <openvpn/client/remotelist.hpp>
#include <openvpn/client/cliconstants.hpp>

namespace openvpn {
  class ParseClientConfig {
  public:
    struct ServerEntry {
      std::string server;
      std::string friendlyName;
    };

    struct ServerList : public std::vector<ServerEntry>
    {
    };

    ParseClientConfig(const OptionList& options)
    {
      try {
	// reset POD types
	reset_pod();

	// limits
	const size_t max_server_list_size = 64;

	// process setenv directives
	{
	  const OptionList::IndexList* se = options.get_index_ptr("setenv");
	  if (se)
	    {
	      for (OptionList::IndexList::const_iterator i = se->begin(); i != se->end(); ++i)
		{
		  const Option& o = options[*i];
		  const std::string arg1 = o.get_optional(1, 256);

		  // server-locked profiles not supported
		  if (arg1 == "GENERIC_CONFIG")
		    {
		      error_ = true;
		      message_ = "SERVER_LOCKED_UNSUPPORTED: server locked profiles are currently unsupported";
		      return;
		    }
		  else if (arg1 == "ALLOW_PASSWORD_SAVE")
		    allowPasswordSave_ = parse_bool(o, "setenv ALLOW_PASSWORD_SAVE", 2);
		  else if (arg1 == "CLIENT_CERT")
		    clientCertEnabled_ = parse_bool(o, "setenv CLIENT_CERT", 2);
		  else if (arg1 == "USERNAME")
		    userlockedUsername_ = o.get(2, 256);
		  else if (arg1 == "FRIENDLY_NAME")
		    friendlyName_ = o.get(2, 256);
		  else if (arg1 == "SERVER")
		    {
		      const std::string& serv = o.get(2, 256);
		      std::vector<std::string> slist = Split::by_char<std::vector<std::string>, NullLex, Split::NullLimit>(serv, '/', 0, 1);
		      ServerEntry se;
		      if (slist.size() == 1)
			{
			  se.server = slist[0];
			  se.friendlyName = slist[0];
			}
		      else if (slist.size() == 2)
			{
			  se.server = slist[0];
			  se.friendlyName = slist[1];
			}
		      if (!se.server.empty() && !se.friendlyName.empty() && serverList_.size() < max_server_list_size)
			serverList_.push_back(se);
		    }
		}
	    }
	}

	// Alternative to "setenv CLIENT_CERT 0".  Note that as of OpenVPN 2.3, this option
	// is only supported server-side, so this extends its meaning into the client realm.
	if (options.exists("client-cert-not-required"))
	  clientCertEnabled_ = false;

	// userlocked username
	{
	  const Option* o = options.get_ptr("USERNAME");
	  if (o)
	    userlockedUsername_ = o->get(1, 256);
	}

	// External PKI
	externalPki_ = (clientCertEnabled_ && is_external_pki(options));

	// allow password save
	{
	  const Option* o = options.get_ptr("allow-password-save");
	  if (o)
	    allowPasswordSave_ = parse_bool(*o, "allow-password-save", 1);
	}

	// autologin
	{
	  autologin_ = is_autologin(options);
	  if (autologin_)
	    allowPasswordSave_ = false; // saving passwords is incompatible with autologin
	}

	// static challenge
	{
	  const Option* o = options.get_ptr("static-challenge");
	  if (o)
	    {
	      staticChallenge_ = o->get(1, 256);
	      if (o->get_optional(2, 16) == "1")
		staticChallengeEcho_ = true;
	    }
	}

	// validate remote list
	RemoteList rl(options);

	// determine if private key is encrypted
	if (!externalPki_)
	  {
	    const Option* o = options.get_ptr("key");
	    if (o)
	      {
		const std::string& key_txt = o->get(1, Option::MULTILINE);
		privateKeyPasswordRequired_ = (
	            key_txt.find("-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\n") != std::string::npos
		 || key_txt.find("-----BEGIN ENCRYPTED PRIVATE KEY-----") != std::string::npos
		);
	      }
	  }

	// profile name
	{
	  const Option* o = options.get_ptr("PROFILE");
	  if (o)
	    {
	      // take PROFILE substring up to '/'
	      const std::string& pn = o->get(1, 256);
	      const size_t slashpos = pn.find('/');
	      if (slashpos != std::string::npos)
		profileName_ = pn.substr(0, slashpos);
	      else
		profileName_ = pn;
	    }
	  else
	    {
	      if (rl.defined())
		profileName_ = rl.first_server_host();
	    }
	}

	// friendly name
	{
	  const Option* o = options.get_ptr("FRIENDLY_NAME");
	  if (o)
	    friendlyName_ = o->get(1, 256);
	}

	// server list
	{
	  const Option* o = options.get_ptr("HOST_LIST");
	  if (o)
	    {
	      SplitLines in(o->get(1, 4096 | Option::MULTILINE), 0);
	      while (in(true))
		{
		  ServerEntry se;
		  se.server = in.line_ref();
		  se.friendlyName = se.server;
		  Option::validate_string("HOST_LIST server", se.server, 256);
		  Option::validate_string("HOST_LIST friendly name", se.friendlyName, 256);
		  if (!se.server.empty() && !se.friendlyName.empty() && serverList_.size() < max_server_list_size)
		    serverList_.push_back(se);
		}
	    }
	}
      }
      catch (const std::exception& e)
	{
	  error_ = true;
	  message_ = Unicode::utf8_printable(e.what(), 256);
	}
    }

    static ParseClientConfig parse(const std::string& content, OptionList::KeyValueList* content_list)
    {
      OptionList options;
      return parse(content, content_list, options);
    }

    static ParseClientConfig parse(const std::string& content,
				   OptionList::KeyValueList* content_list,
				   OptionList& options)
    {
      try {
	OptionList::Limits limits("profile is too large",
				  ProfileParseLimits::MAX_PROFILE_SIZE,
				  ProfileParseLimits::OPT_OVERHEAD,
				  ProfileParseLimits::TERM_OVERHEAD,
				  ProfileParseLimits::MAX_LINE_SIZE,
				  ProfileParseLimits::MAX_DIRECTIVE_SIZE);
	options.clear();
	options.parse_from_config(content, &limits);
	options.parse_meta_from_config(content, "OVPN_ACCESS_SERVER", &limits);
	if (content_list)
	  {
	    content_list->preprocess();
	    options.parse_from_key_value_list(*content_list, &limits);
	  }
	process_setenv_opt(options);
	options.update_map();

	// add in missing options
	bool added = false;

	// client
	if (!options.exists("client"))
	  {
	    Option opt;
	    opt.push_back("client");
	    options.push_back(opt);
	    added = true;
	  }

	// dev
	if (!options.exists("dev"))
	  {
	    Option opt;
	    opt.push_back("dev");
	    opt.push_back("tun");
	    options.push_back(opt);
	    added = true;
	  }
	if (added)
	  options.update_map();

	return ParseClientConfig(options);
      }
      catch (const std::exception& e)
	{
	  ParseClientConfig ret;
	  ret.error_ = true;
	  ret.message_ = Unicode::utf8_printable(e.what(), 256);
          return ret;
	}
    }

    // true if error
    bool error() const { return error_; }

    // if error, message given here
    const std::string& message() const { return message_; }

    // this username must be used with profile
    const std::string& userlockedUsername() const { return userlockedUsername_; }

    // profile name of config
    const std::string& profileName() const { return profileName_; }

    // "friendly" name of config
    const std::string& friendlyName() const { return friendlyName_; }

    // true: no creds required, false: username/password required
    bool autologin() const { return autologin_; }

    // true: no client cert/key required, false: client cert/key required
    bool clientCertEnabled() const { return clientCertEnabled_; }

    // if true, this is an External PKI profile (no cert or key directives)
    bool externalPki() const { return externalPki_; }

    // static challenge, may be empty, ignored if autologin
    const std::string& staticChallenge() const { return staticChallenge_; }

    // true if static challenge response should be echoed to UI, ignored if autologin
    bool staticChallengeEcho() const { return staticChallengeEcho_; }

    // true if this profile requires a private key password
    bool privateKeyPasswordRequired() const { return privateKeyPasswordRequired_; }

    // true if user is allowed to save authentication password in UI
    bool allowPasswordSave() const { return allowPasswordSave_; }

    // optional list of user-selectable VPN servers
    const ServerList& serverList() const { return serverList_; }

    std::string to_string() const
    {
      std::ostringstream os;
      os << "user=" << userlockedUsername_
	 << " pn=" << profileName_
	 << " fn=" << friendlyName_
	 << " auto=" << autologin_
	 << " epki=" << externalPki_
	 << " schal=" << staticChallenge_
	 << " scecho=" << staticChallengeEcho_;
      return os.str();
    }

  private:
    static void process_setenv_opt(OptionList& options)
    {
      for (OptionList::iterator i = options.begin(); i != options.end(); ++i)
	{
	  Option& o = *i;
	  if (o.size() >= 3 && o.ref(0) == "setenv" && o.ref(1) == "opt")
	    o.remove_first(2);
	}
    }

    static bool is_autologin(const OptionList& options)
    {
      const Option* autologin = options.get_ptr("AUTOLOGIN");
      if (autologin)
	return string::is_true(autologin->get_optional(1, 16));
      else
	{
	  const Option* auth_user_pass = options.get_ptr("auth-user-pass");
	  bool ret = !auth_user_pass;
	  if (ret)
	    {
	      // External PKI profiles from AS don't declare auth-user-pass,
	      // and we have no way of knowing if they are autologin unless
	      // we examine their cert, which requires accessing the system-level
	      // cert store on the client.  For now, we are going to assume
	      // that External PKI profiles from the AS are always userlogin,
	      // unless explicitly overriden by AUTOLOGIN above.
	      if (options.exists("EXTERNAL_PKI"))
		return false;
	    }
	  return ret;
	}
    }

    static bool is_external_pki(const OptionList& options)
    {
      const Option* epki = options.get_ptr("EXTERNAL_PKI");
      if (epki)
	return string::is_true(epki->get_optional(1, 16));
      else
	{
	  const Option* cert = options.get_ptr("cert");
	  const Option* key = options.get_ptr("key");
	  return !cert || !key;
	}
    }

    ParseClientConfig()
    {
      reset_pod();
    }

    void reset_pod()
    {
      error_ = autologin_ = externalPki_ = staticChallengeEcho_ = privateKeyPasswordRequired_ = false;
      allowPasswordSave_ = clientCertEnabled_ = true;
    }

    bool parse_bool(const Option& o, const std::string& title, const size_t index)
    {
      const std::string parm = o.get(index, 16);
      if (parm == "0")
	return false;
      else if (parm == "1")
	return true;
      else
	throw option_error(title + ": parameter must be 0 or 1");
    }

    bool error_;
    std::string message_;
    std::string userlockedUsername_;
    std::string profileName_;
    std::string friendlyName_;
    bool autologin_;
    bool clientCertEnabled_;
    bool externalPki_;
    std::string staticChallenge_;
    bool staticChallengeEcho_;
    bool privateKeyPasswordRequired_;
    bool allowPasswordSave_;
    ServerList serverList_;
  };
}

#endif
