/*
 * Copyright (C) 1996-2024 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_SRC_ACL_PROTOCOL_H
#define SQUID_SRC_ACL_PROTOCOL_H

#include "acl/Strategy.h"
#include "anyp/ProtocolType.h"

class ACLProtocolStrategy : public ACLStrategy<AnyP::ProtocolType>
{

public:
    int match (ACLData<MatchType> * &, ACLFilledChecklist *) override;
    bool requiresRequest() const override {return true;}
};

#endif /* SQUID_SRC_ACL_PROTOCOL_H */

