/*
 * Copyright (C) 1996-2024 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_SRC_ACL_HASCOMPONENT_H
#define SQUID_SRC_ACL_HASCOMPONENT_H

#include "acl/Strategised.h"
#include "acl/Strategy.h"

/// \ingroup ACLAPI
class ACLHasComponentStrategy : public ACLStrategy<ACLChecklist *>
{
public:
    int match(ACLData<MatchType> * &, ACLFilledChecklist *) override;
};

#endif /* SQUID_SRC_ACL_HASCOMPONENT_H */

