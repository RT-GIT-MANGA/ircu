/*
 * IRC - Internet Relay Chat, ircd/channel.c
 * Copyright (C) 1990 Jarkko Oikarinen and
 *                    University of Oulu, Co Center
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id$
 */
#include "channel.h"
#include "client.h"
#include "hash.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_chattr.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "list.h"
#include "match.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "querycmds.h"
#include "s_bsd.h"
#include "s_conf.h"
#include "s_debug.h"
#include "s_misc.h"
#include "s_user.h"
#include "send.h"
#include "sprintf_irc.h"
#include "struct.h"
#include "support.h"
#include "sys.h"
#include "whowas.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Channel* GlobalChannelList = 0;

static struct SLink *next_overlapped_ban(void);
static int del_banid(struct Channel *, char *, int);
void del_invite(struct Client *, struct Channel *);

const char* const PartFmt1     = ":%s " MSG_PART " %s";
const char* const PartFmt2     = ":%s " MSG_PART " %s :%s";
const char* const PartFmt1serv = "%s%s " TOK_PART " %s";
const char* const PartFmt2serv = "%s%s " TOK_PART " %s :%s";


static struct SLink* next_ban;
static struct SLink* prev_ban;
static struct SLink* removed_bans_list;

/*
 * Use a global variable to remember if an oper set a mode on a local channel. Ugly,
 * but the only way to do it without changing set_mode intensively.
 */
int LocalChanOperMode = 0;

#if !defined(NDEBUG)
/*
 * return the length (>=0) of a chain of links.
 */
static int list_length(struct SLink *lp)
{
  int count = 0;

  for (; lp; lp = lp->next)
    ++count;
  return count;
}
#endif

struct Membership* find_member_link(struct Channel* chptr, const struct Client* cptr)
{
  struct Membership *m;
  assert(0 != cptr);
  assert(0 != chptr);
  
  /* Servers don't have member links */
  if (IsServer(cptr))
     return 0;
  
  /* +k users are typically on a LOT of channels.  So we iterate over who
   * is in the channel.  X/W are +k and are in about 5800 channels each.
   * however there are typically no more than 1000 people in a channel
   * at a time.
   */
  if (IsChannelService(cptr)) {
    m = chptr->members;
    while (m) {
      assert(m->channel == chptr);
      if (m->user == cptr)
        return m;
      m = m->next_member;
    }
  }
  /* Users on the other hand aren't allowed on more than 15 channels.  50%
   * of users that are on channels are on 2 or less, 95% are on 7 or less,
   * and 99% are on 10 or less.
   */
  else {
   m = cptr->user->channel;
   while (m) {
     assert(m->user == cptr);
     if (m->channel == chptr)
       return m;
     m = m->next_channel;
   }
  }
  return 0;
}

/*
 * find_chasing - Find the client structure for a nick name (user)
 * using history mechanism if necessary. If the client is not found, an error
 * message (NO SUCH NICK) is generated. If the client was found
 * through the history, chasing will be 1 and otherwise 0.
 */
struct Client* find_chasing(struct Client* sptr, const char* user, int* chasing)
{
  struct Client* who = FindClient(user);

  if (chasing)
    *chasing = 0;
  if (who)
    return who;

  if (!(who = get_history(user, KILLCHASETIMELIMIT))) {
    send_reply(sptr, ERR_NOSUCHNICK, user);
    return 0;
  }
  if (chasing)
    *chasing = 1;
  return who;
}

/*
 * Create a string of form "foo!bar@fubar" given foo, bar and fubar
 * as the parameters.  If NULL, they become "*".
 */
static char *make_nick_user_host(const char *nick, const char *name,
                                 const char *host)
{
  static char namebuf[NICKLEN + USERLEN + HOSTLEN + 3];
  sprintf_irc(namebuf, "%s!%s@%s", nick, name, host);
  return namebuf;
}

/*
 * Create a string of form "foo!bar@123.456.789.123" given foo, bar and the
 * IP-number as the parameters.  If NULL, they become "*".
 */
static char *make_nick_user_ip(char *nick, char *name, struct in_addr ip)
{
  static char ipbuf[NICKLEN + USERLEN + 16 + 3];
  sprintf_irc(ipbuf, "%s!%s@%s", nick, name, ircd_ntoa((const char*) &ip));
  return ipbuf;
}

#if 0
static int DoesOp(const char* modebuf)
{
  assert(0 != modebuf);
  while (*modebuf) {
    if (*modebuf == 'o' || *modebuf == 'v')
      return 1;
    ++modebuf;
  }
  return 0;
}

/*
 * This function should be removed when all servers are 2.10
 */
static void sendmodeto_one(struct Client* cptr, const char* from,
                           const char* name, const char* mode,
                           const char* param, time_t creationtime)
{
  if (IsServer(cptr) && DoesOp(mode) && creationtime)
    sendto_one(cptr, ":%s MODE %s %s %s " TIME_T_FMT, /* XXX DEAD */
               from, name, mode, param, creationtime);
  else
    sendto_one(cptr, ":%s MODE %s %s %s", from, name, mode, param); /* XXX DEAD */
}
#endif /* 0 */

/*
 * Subtract one user from channel i (and free channel
 * block, if channel became empty).
 * Returns: true  (1) if channel still exists
 *          false (0) if the channel was destroyed
 */
int sub1_from_channel(struct Channel* chptr)
{
  struct SLink *tmp;
  struct SLink *obtmp;

  if (chptr->users > 1)         /* Can be 0, called for an empty channel too */
  {
    assert(0 != chptr->members);
    --chptr->users;
    return 1;
  }

  assert(0 == chptr->members);

  /* Channel became (or was) empty: Remove channel */
  if (is_listed(chptr))
  {
    int i;
    for (i = 0; i <= HighestFd; i++)
    {
      struct Client *acptr;
      if ((acptr = LocalClientArray[i]) && acptr->listing &&
          acptr->listing->chptr == chptr)
      {
        list_next_channels(acptr, 1);
        break;                  /* Only one client can list a channel */
      }
    }
  }
  /*
   * Now, find all invite links from channel structure
   */
  while ((tmp = chptr->invites))
    del_invite(tmp->value.cptr, chptr);

  tmp = chptr->banlist;
  while (tmp)
  {
    obtmp = tmp;
    tmp = tmp->next;
    MyFree(obtmp->value.ban.banstr);
    MyFree(obtmp->value.ban.who);
    free_link(obtmp);
  }
  if (chptr->prev)
    chptr->prev->next = chptr->next;
  else
    GlobalChannelList = chptr->next;
  if (chptr->next)
    chptr->next->prev = chptr->prev;
  hRemChannel(chptr);
  --UserStats.channels;
  /*
   * make sure that channel actually got removed from hash table
   */
  assert(chptr->hnext == chptr);
  MyFree(chptr);
  return 0;
}

/*
 * add_banid
 *
 * `cptr' must be the client adding the ban.
 *
 * If `change' is true then add `banid' to channel `chptr'.
 * Returns 0 if the ban was added.
 * Returns -2 if the ban already existed and was marked CHFL_BURST_BAN_WIPEOUT.
 * Return -1 otherwise.
 *
 * Those bans that overlapped with `banid' are flagged with CHFL_BAN_OVERLAPPED
 * when `change' is false, otherwise they will be removed from the banlist.
 * Subsequently calls to next_overlapped_ban() or next_removed_overlapped_ban()
 * respectively will return these bans until NULL is returned.
 *
 * If `firsttime' is true, the ban list as returned by next_overlapped_ban()
 * is reset (unless a non-zero value is returned, in which case the
 * CHFL_BAN_OVERLAPPED flag might not have been reset!).
 *
 * --Run
 */
int add_banid(struct Client *cptr, struct Channel *chptr, char *banid,
                     int change, int firsttime)
{
  struct SLink*  ban;
  struct SLink** banp;
  int            cnt = 0;
  int            removed_bans = 0;
  int            len = strlen(banid);

  if (firsttime)
  {
    next_ban = NULL;
    assert(0 == prev_ban);
    assert(0 == removed_bans_list);
  }
  if (MyUser(cptr))
    collapse(banid);
  for (banp = &chptr->banlist; *banp;)
  {
    len += strlen((*banp)->value.ban.banstr);
    ++cnt;
    if (((*banp)->flags & CHFL_BURST_BAN_WIPEOUT))
    {
      if (!strcmp((*banp)->value.ban.banstr, banid))
      {
        (*banp)->flags &= ~CHFL_BURST_BAN_WIPEOUT;
        return -2;
      }
    }
    else if (!mmatch((*banp)->value.ban.banstr, banid))
      return -1;
    if (!mmatch(banid, (*banp)->value.ban.banstr))
    {
      struct SLink *tmp = *banp;
      if (change)
      {
        if (MyUser(cptr))
        {
          cnt--;
          len -= strlen(tmp->value.ban.banstr);
        }
        *banp = tmp->next;
#if 0
        /* Silently remove overlapping bans */
        MyFree(tmp->value.ban.banstr);
        MyFree(tmp->value.ban.who);
        free_link(tmp);
        tmp = 0;
#else
        /* These will be sent to the user later as -b */
        tmp->next = removed_bans_list;
        removed_bans_list = tmp;
        removed_bans = 1;
#endif
      }
      else if (!(tmp->flags & CHFL_BURST_BAN_WIPEOUT))
      {
        tmp->flags |= CHFL_BAN_OVERLAPPED;
        if (!next_ban)
          next_ban = tmp;
        banp = &tmp->next;
      }
      else
        banp = &tmp->next;
    }
    else
    {
      if (firsttime)
        (*banp)->flags &= ~CHFL_BAN_OVERLAPPED;
      banp = &(*banp)->next;
    }
  }
  if (MyUser(cptr) && !removed_bans && (len > MAXBANLENGTH || (cnt >= MAXBANS)))
  {
    send_reply(cptr, ERR_BANLISTFULL, chptr->chname, banid);
    return -1;
  }
  if (change)
  {
    char*              ip_start;
    struct Membership* member;
    ban = make_link();
    ban->next = chptr->banlist;

    ban->value.ban.banstr = (char*) MyMalloc(strlen(banid) + 1);
    assert(0 != ban->value.ban.banstr);
    strcpy(ban->value.ban.banstr, banid);

    ban->value.ban.who = (char*) MyMalloc(strlen(cptr->name) + 1);
    assert(0 != ban->value.ban.who);
    strcpy(ban->value.ban.who, cptr->name);

    ban->value.ban.when = TStime();
    ban->flags = CHFL_BAN;      /* This bit is never used I think... */
    if ((ip_start = strrchr(banid, '@')) && check_if_ipmask(ip_start + 1))
      ban->flags |= CHFL_BAN_IPMASK;
    chptr->banlist = ban;

    /*
     * Erase ban-valid-bit
     */
    for (member = chptr->members; member; member = member->next_member)
      ClearBanValid(member);     /* `ban' == channel member ! */
  }
  return 0;
}

static struct SLink *next_overlapped_ban(void)
{
  struct SLink *tmp = next_ban;
  if (tmp)
  {
    struct SLink *ban;
    for (ban = tmp->next; ban; ban = ban->next)
      if ((ban->flags & CHFL_BAN_OVERLAPPED))
        break;
    next_ban = ban;
  }
  return tmp;
}

struct SLink *next_removed_overlapped_ban(void)
{
  struct SLink *tmp = removed_bans_list;
  if (prev_ban)
  {
    if (prev_ban->value.ban.banstr)     /* Can be set to NULL in set_mode() */
      MyFree(prev_ban->value.ban.banstr);
    MyFree(prev_ban->value.ban.who);
    free_link(prev_ban);
    prev_ban = 0;
  }
  if (tmp)
    removed_bans_list = removed_bans_list->next;
  prev_ban = tmp;
  return tmp;
}

/*
 * del_banid
 *
 * If `change' is true, delete `banid' from channel `chptr'.
 * Returns `false' if removal was (or would have been) successful.
 */
static int del_banid(struct Channel *chptr, char *banid, int change)
{
  struct SLink **ban;
  struct SLink *tmp;

  if (!banid)
    return -1;
  for (ban = &(chptr->banlist); *ban; ban = &((*ban)->next)) {
    if (0 == ircd_strcmp(banid, (*ban)->value.ban.banstr))
    {
      tmp = *ban;
      if (change)
      {
        struct Membership* member;
        *ban = tmp->next;
        MyFree(tmp->value.ban.banstr);
        MyFree(tmp->value.ban.who);
        free_link(tmp);
        /*
         * Erase ban-valid-bit, for channel members that are banned
         */
        for (member = chptr->members; member; member = member->next_member)
          if (CHFL_BANVALIDMASK == (member->status & CHFL_BANVALIDMASK))
            ClearBanValid(member);       /* `tmp' == channel member */
      }
      return 0;
    }
  }
  return -1;
}

/*
 * find_channel_member - returns Membership * if a person is joined and not a zombie
 */
struct Membership* find_channel_member(struct Client* cptr, struct Channel* chptr)
{
  struct Membership* member;
  assert(0 != chptr);

  member = find_member_link(chptr, cptr);
  return (member && !IsZombie(member)) ? member : 0;
}

/*
 * is_banned - a non-zero value if banned else 0.
 */
static int is_banned(struct Client *cptr, struct Channel *chptr,
                     struct Membership* member)
{
  struct SLink* tmp;
  char*         s;
  char*         ip_s = NULL;

  if (!IsUser(cptr))
    return 0;

  if (member && IsBanValid(member))
    return IsBanned(member);

  s = make_nick_user_host(cptr->name, cptr->user->username, cptr->user->host);

  for (tmp = chptr->banlist; tmp; tmp = tmp->next) {
    if ((tmp->flags & CHFL_BAN_IPMASK)) {
      if (!ip_s)
        ip_s = make_nick_user_ip(cptr->name, cptr->user->username, cptr->ip);
      if (match(tmp->value.ban.banstr, ip_s) == 0)
        break;
    }
    else if (match(tmp->value.ban.banstr, s) == 0)
      break;
  }

  if (member) {
    SetBanValid(member);
    if (tmp) {
      SetBanned(member);
      return 1;
    }
    else {
      ClearBanned(member);
      return 0;
    }
  }

  return (tmp != NULL);
}

/*
 * adds a user to a channel by adding another link to the channels member
 * chain.
 */
void add_user_to_channel(struct Channel* chptr, struct Client* who,
                                unsigned int flags)
{
  assert(0 != chptr);
  assert(0 != who);

  if (who->user) {
    struct Membership* member = 
            (struct Membership*) MyMalloc(sizeof(struct Membership));
    assert(0 != member);
    member->user         = who;
    member->channel      = chptr;
    member->status       = flags;

    member->next_member  = chptr->members;
    if (member->next_member)
      member->next_member->prev_member = member;
    member->prev_member  = 0; 
    chptr->members       = member;

    member->next_channel = who->user->channel;
    if (member->next_channel)
      member->next_channel->prev_channel = member;
    member->prev_channel = 0;
    who->user->channel = member;

    ++chptr->users;
    ++who->user->joined;
  }
}

static int remove_member_from_channel(struct Membership* member)
{
  struct Channel* chptr;
  assert(0 != member);
  chptr = member->channel;
  /*
   * unlink channel member list
   */
  if (member->next_member)
    member->next_member->prev_member = member->prev_member;
  if (member->prev_member)
    member->prev_member->next_member = member->next_member;
  else
    member->channel->members = member->next_member; 
      
  /*
   * unlink client channel list
   */
  if (member->next_channel)
    member->next_channel->prev_channel = member->prev_channel;
  if (member->prev_channel)
    member->prev_channel->next_channel = member->next_channel;
  else
    member->user->user->channel = member->next_channel;

  --member->user->user->joined;
  MyFree(member);

  return sub1_from_channel(chptr);
}

static int channel_all_zombies(struct Channel* chptr)
{
  struct Membership* member;

  for (member = chptr->members; member; member = member->next_member) {
    if (!IsZombie(member))
      return 0;
  }
  return 1;
}
      

void remove_user_from_channel(struct Client* cptr, struct Channel* chptr)
{
  
  struct Membership* member;
  assert(0 != chptr);

  if ((member = find_member_link(chptr, cptr))) {
    if (remove_member_from_channel(member)) {
      if (channel_all_zombies(chptr)) {
        /*
         * XXX - this looks dangerous but isn't if we got the referential
         * integrity right for channels
         */
        while (remove_member_from_channel(chptr->members))
          ;
      }
    }
  }
}

void remove_user_from_all_channels(struct Client* cptr)
{
  struct Membership* chan;
  assert(0 != cptr);
  assert(0 != cptr->user);

  while ((chan = cptr->user->channel))
    remove_user_from_channel(cptr, chan->channel);
}

int is_chan_op(struct Client *cptr, struct Channel *chptr)
{
  struct Membership* member;
  assert(chptr);
  if ((member = find_member_link(chptr, cptr)))
    return (!IsZombie(member) && IsChanOp(member));

  return 0;
}

static int is_deopped(struct Client *cptr, struct Channel *chptr)
{
  struct Membership* member;

  assert(0 != chptr);
  if ((member = find_member_link(chptr, cptr)))
    return IsDeopped(member);

  return (IsUser(cptr) ? 1 : 0);
}

int is_zombie(struct Client *cptr, struct Channel *chptr)
{
  struct Membership* member;

  assert(0 != chptr);

  if ((member = find_member_link(chptr, cptr)))
      return IsZombie(member);
  return 0;
}

int has_voice(struct Client* cptr, struct Channel* chptr)
{
  struct Membership* member;

  assert(0 != chptr);
  if ((member = find_member_link(chptr, cptr)))
    return (!IsZombie(member) && HasVoice(member));

  return 0;
}

int member_can_send_to_channel(struct Membership* member)
{
  assert(0 != member);

  if (IsVoicedOrOpped(member))
    return 1;
  /*
   * If it's moderated, and you aren't a priviledged user, you can't
   * speak.  
   */
  if (member->channel->mode.mode & MODE_MODERATED)
    return 0;
  /*
   * If you're banned then you can't speak either.
   * but because of the amount of CPU time that is_banned chews
   * we only check it for our clients.
   */
  if (MyUser(member->user) && is_banned(member->user, member->channel, member))
    return 0;
  return 1;
}

int client_can_send_to_channel(struct Client *cptr, struct Channel *chptr)
{
  struct Membership *member;
  assert(0 != cptr); 
  /*
   * Servers can always speak on channels.
   */
  if (IsServer(cptr))
    return 1;

  member = find_channel_member(cptr, chptr);

  /*
   * You can't speak if your off channel, if the channel is modeless, or
   * +n.(no external messages)
   */
  if (!member) {
    if ((chptr->mode.mode & MODE_NOPRIVMSGS) || IsModelessChannel(chptr->chname)) 
      return 0;
    else
      return 1;
  }
  return member_can_send_to_channel(member); 
}

/*
 * find_no_nickchange_channel
 * if a member and not opped or voiced and banned
 * return the name of the first channel banned on
 */
const char* find_no_nickchange_channel(struct Client* cptr)
{
  if (MyUser(cptr)) {
    struct Membership* member;
    for (member = cptr->user->channel; member; member = member->next_channel) {
      if (!IsVoicedOrOpped(member) && is_banned(cptr, member->channel, member))
        return member->channel->chname;
    }
  }
  return 0;
}


/*
 * write the "simple" list of channel modes for channel chptr onto buffer mbuf
 * with the parameters in pbuf.
 */
void channel_modes(struct Client *cptr, char *mbuf, char *pbuf,
                          struct Channel *chptr)
{
  assert(0 != mbuf);
  assert(0 != pbuf);
  assert(0 != chptr);

  *mbuf++ = '+';
  if (chptr->mode.mode & MODE_SECRET)
    *mbuf++ = 's';
  else if (chptr->mode.mode & MODE_PRIVATE)
    *mbuf++ = 'p';
  if (chptr->mode.mode & MODE_MODERATED)
    *mbuf++ = 'm';
  if (chptr->mode.mode & MODE_TOPICLIMIT)
    *mbuf++ = 't';
  if (chptr->mode.mode & MODE_INVITEONLY)
    *mbuf++ = 'i';
  if (chptr->mode.mode & MODE_NOPRIVMSGS)
    *mbuf++ = 'n';
  if (chptr->mode.limit) {
    *mbuf++ = 'l';
    sprintf_irc(pbuf, "%d", chptr->mode.limit);
  }

  if (*chptr->mode.key) {
    *mbuf++ = 'k';
    if (is_chan_op(cptr, chptr) || IsServer(cptr)) {
      if (chptr->mode.limit)
        strcat(pbuf, " ");
      strcat(pbuf, chptr->mode.key);
    }
  }
  *mbuf = '\0';
}

#if 0
static int send_mode_list(struct Client *cptr, char *chname,
                          time_t creationtime, struct SLink *top,
                          int mask, char flag)
{
  struct SLink* lp;
  char*         cp;
  char*         name;
  int           count = 0;
  int           send = 0;
  int           sent = 0;

  cp = modebuf + strlen(modebuf);
  if (*parabuf)                 /* mode +l or +k xx */
    count = 1;
  for (lp = top; lp; lp = lp->next)
  {
    if (!(lp->flags & mask))
      continue;
    if (mask == CHFL_BAN)
      name = lp->value.ban.banstr;
    else
      name = lp->value.cptr->name;
    if (strlen(parabuf) + strlen(name) + 11 < MODEBUFLEN)
    {
      strcat(parabuf, " ");
      strcat(parabuf, name);
      count++;
      *cp++ = flag;
      *cp = '\0';
    }
    else if (*parabuf)
      send = 1;
    if (count == 6)
      send = 1;
    if (send)
    {
      /* cptr is always a server! So we send creationtimes */
      sendmodeto_one(cptr, me.name, chname, modebuf, parabuf, creationtime);
      sent = 1;
      send = 0;
      *parabuf = '\0';
      cp = modebuf;
      *cp++ = '+';
      if (count != 6)
      {
        strcpy(parabuf, name);
        *cp++ = flag;
      }
      count = 0;
      *cp = '\0';
    }
  }
  return sent;
}

#endif /* 0 */

/*
 * send "cptr" a full list of the modes for channel chptr.
 */
void send_channel_modes(struct Client *cptr, struct Channel *chptr)
{
  static unsigned int current_flags[4] =
      { 0, CHFL_CHANOP | CHFL_VOICE, CHFL_VOICE, CHFL_CHANOP };
  int                first = 1;
  int                full  = 1;
  int                flag_cnt = 0;
  int                new_mode = 0;
  size_t             len;
  size_t             sblen;
  struct Membership* member;
  struct SLink*      lp2;
  char modebuf[MODEBUFLEN];
  char parabuf[MODEBUFLEN];

  assert(0 != cptr);
  assert(0 != chptr); 

  if (IsLocalChannel(chptr->chname))
    return;

  member = chptr->members;
  lp2 = chptr->banlist;

  *modebuf = *parabuf = '\0';
  channel_modes(cptr, modebuf, parabuf, chptr);

  for (first = 1; full; first = 0)      /* Loop for multiple messages */
  {
    full = 0;                   /* Assume by default we get it
                                 all in one message */

    /* (Continued) prefix: "<Y> B <channel> <TS>" */
    sprintf_irc(sendbuf, "%s B %s " TIME_T_FMT, NumServ(&me),
                chptr->chname, chptr->creationtime);
    sblen = strlen(sendbuf);

    if (first && modebuf[1])    /* Add simple modes (iklmnpst)
                                 if first message */
    {
      /* prefix: "<Y> B <channel> <TS>[ <modes>[ <params>]]" */
      sendbuf[sblen++] = ' ';
      strcpy(sendbuf + sblen, modebuf);
      sblen += strlen(modebuf);
      if (*parabuf)
      {
        sendbuf[sblen++] = ' ';
        strcpy(sendbuf + sblen, parabuf);
        sblen += strlen(parabuf);
      }
    }

    /*
     * Attach nicks, comma seperated " nick[:modes],nick[:modes],..."
     *
     * Run 4 times over all members, to group the members with the
     * same mode together
     */
    for (first = 1; flag_cnt < 4;
         member = chptr->members, new_mode = 1, flag_cnt++)
    {
      for (; member; member = member->next_member)
      {
        if ((member->status & CHFL_VOICED_OR_OPPED) !=
            current_flags[flag_cnt])
          continue;             /* Skip members with different flags */
        if (sblen + NUMNICKLEN + 4 > BUFSIZE - 3)
          /* The 4 is a possible ",:ov"
             The -3 is for the "\r\n\0" that is added in send.c */
        {
          full = 1;           /* Make sure we continue after
                                 sending it so far */
          new_mode = 1;       /* Ensure the new BURST line contains the current
                                 mode. --Gte */
          break;              /* Do not add this member to this message */
        }
        sendbuf[sblen++] = first ? ' ' : ',';
        first = 0;              /* From now on, us comma's to add new nicks */

        sprintf_irc(sendbuf + sblen, "%s%s", NumNick(member->user));
        sblen += strlen(sendbuf + sblen);
        /*
         * Do we have a nick with a new mode ?
         * Or are we starting a new BURST line?
         */
        if (new_mode)
        {
          new_mode = 0;
          if (IsVoicedOrOpped(member)) {
            sendbuf[sblen++] = ':';
            if (IsChanOp(member))
              sendbuf[sblen++] = 'o';
            if (HasVoice(member))
              sendbuf[sblen++] = 'v';
          }
        }
      }
      if (full)
        break;
    }

    if (!full)
    {
      /* Attach all bans, space seperated " :%ban ban ..." */
      for (first = 2; lp2; lp2 = lp2->next)
      {
        len = strlen(lp2->value.ban.banstr);
        if (sblen + len + 1 + first > BUFSIZE - 3)
          /* The +1 stands for the added ' '.
           * The +first stands for the added ":%".
           * The -3 is for the "\r\n\0" that is added in send.c
           */
        {
          full = 1;
          break;
        }
        if (first)
        {
          first = 0;
          sendbuf[sblen++] = ' ';
          sendbuf[sblen++] = ':';       /* Will be last parameter */
          sendbuf[sblen++] = '%';       /* To tell bans apart */
        }
        else
          sendbuf[sblen++] = ' ';
        strcpy(sendbuf + sblen, lp2->value.ban.banstr);
        sblen += len;
      }
    }

    sendbuf[sblen] = '\0';
    sendbufto_one(cptr);        /* Send this message */
  }                             /* Continue when there was something
                                 that didn't fit (full==1) */
}

/*
 * pretty_mask
 *
 * by Carlo Wood (Run), 05 Oct 1998.
 *
 * Canonify a mask.
 *
 * When the nick is longer then NICKLEN, it is cut off (its an error of course).
 * When the user name or host name are too long (USERLEN and HOSTLEN
 * respectively) then they are cut off at the start with a '*'.
 *
 * The following transformations are made:
 *
 * 1)   xxx             -> nick!*@*
 * 2)   xxx.xxx         -> *!*@host
 * 3)   xxx!yyy         -> nick!user@*
 * 4)   xxx@yyy         -> *!user@host
 * 5)   xxx!yyy@zzz     -> nick!user@host
 */
char *pretty_mask(char *mask)
{
  static char star[2] = { '*', 0 };
  char *last_dot = NULL;
  char *ptr;

  /* Case 1: default */
  char *nick = mask;
  char *user = star;
  char *host = star;

  /* Do a _single_ pass through the characters of the mask: */
  for (ptr = mask; *ptr; ++ptr)
  {
    if (*ptr == '!')
    {
      /* Case 3 or 5: Found first '!' (without finding a '@' yet) */
      user = ++ptr;
      host = star;
    }
    else if (*ptr == '@')
    {
      /* Case 4: Found last '@' (without finding a '!' yet) */
      nick = star;
      user = mask;
      host = ++ptr;
    }
    else if (*ptr == '.')
    {
      /* Case 2: Found last '.' (without finding a '!' or '@' yet) */
      last_dot = ptr;
      continue;
    }
    else
      continue;
    for (; *ptr; ++ptr)
    {
      if (*ptr == '@')
      {
        /* Case 4 or 5: Found last '@' */
        host = ptr + 1;
      }
    }
    break;
  }
  if (user == star && last_dot)
  {
    /* Case 2: */
    nick = star;
    user = star;
    host = mask;
  }
  /* Check lengths */
  if (nick != star)
  {
    char *nick_end = (user != star) ? user - 1 : ptr;
    if (nick_end - nick > NICKLEN)
      nick[NICKLEN] = 0;
    *nick_end = 0;
  }
  if (user != star)
  {
    char *user_end = (host != star) ? host - 1 : ptr;
    if (user_end - user > USERLEN)
    {
      user = user_end - USERLEN;
      *user = '*';
    }
    *user_end = 0;
  }
  if (host != star && ptr - host > HOSTLEN)
  {
    host = ptr - HOSTLEN;
    *host = '*';
  }
  return make_nick_user_host(nick, user, host);
}

static void send_ban_list(struct Client* cptr, struct Channel* chptr)
{
  struct SLink* lp;

  assert(0 != cptr);
  assert(0 != chptr);

  for (lp = chptr->banlist; lp; lp = lp->next)
    send_reply(cptr, RPL_BANLIST, chptr->chname, lp->value.ban.banstr,
	       lp->value.ban.who, lp->value.ban.when);

  send_reply(cptr, RPL_ENDOFBANLIST, chptr->chname);
}

/*
 * Check and try to apply the channel modes passed in the parv array for
 * the client ccptr to channel chptr.  The resultant changes are printed
 * into mbuf and pbuf (if any) and applied to the channel.
 */
int set_mode(struct Client* cptr, struct Client* sptr,
                    struct Channel* chptr, int parc, char* parv[],
                    char* mbuf, char* pbuf, char* npbuf, int* badop)
{ 
  /* 
   * This size is only needed when a broken
   * server sends more then MAXMODEPARAMS
   * parameters
   */
  static struct SLink chops[MAXPARA - 2];
  static int flags[] = {
    MODE_PRIVATE,    'p',
    MODE_SECRET,     's',
    MODE_MODERATED,  'm',
    MODE_NOPRIVMSGS, 'n',
    MODE_TOPICLIMIT, 't',
    MODE_INVITEONLY, 'i',
    MODE_VOICE,      'v',
    MODE_KEY,        'k',
    0x0, 0x0
  };

  char bmodebuf[MODEBUFLEN];
  char bparambuf[MODEBUFLEN];
  char nbparambuf[MODEBUFLEN];     /* "Numeric" Bounce Parameter Buffer */
  struct SLink*      lp;
  char*              curr = parv[0];
  char*              cp = NULL;
  int*               ip;
  struct Membership* member_x;
  struct Membership* member_y;
  unsigned int       whatt = MODE_ADD;
  unsigned int       bwhatt = 0;
  int                limitset = 0;
  int                bounce;
  int                add_banid_called = 0;
  size_t             len;
  size_t             nlen;
  size_t             blen;
  size_t             nblen;
  int                keychange = 0;
  unsigned int       nusers = 0;
  unsigned int       newmode;
  int                opcnt = 0;
  int                banlsent = 0;
  int                doesdeop = 0;
  int                doesop = 0;
  int                hacknotice = 0;
  int                change;
  int                gotts = 0;
  struct Client*     who;
  struct Mode*       mode;
  struct Mode        oldm;
  static char        numeric[16];
  char*              bmbuf = bmodebuf;
  char*              bpbuf = bparambuf;
  char*              nbpbuf = nbparambuf;
  time_t             newtime = 0;
  struct ConfItem*   aconf;

  *mbuf = *pbuf = *npbuf = *bmbuf = *bpbuf = *nbpbuf = '\0';
  *badop = 0;
  if (parc < 1)
    return 0;
  /*
   * Mode is accepted when sptr is a channel operator
   * but also when the mode is received from a server.
   * At this point, let any member pass, so they are allowed
   * to see the bans.
   */
  member_y = find_channel_member(sptr, chptr);
  if (!(IsServer(cptr) || member_y))
    return 0;

#ifdef OPER_MODE_LCHAN
  if (IsOperOnLocalChannel(sptr, chptr->chname) && !IsChanOp(member_y))
    LocalChanOperMode = 1;
#endif

  mode = &(chptr->mode);
  memcpy(&oldm, mode, sizeof(struct Mode));

  newmode = mode->mode;

  while (curr && *curr) {
    switch (*curr) {
      case '+':
        whatt = MODE_ADD;
        break;
      case '-':
        whatt = MODE_DEL;
        break;
      case 'o':
      case 'v':
        if (--parc <= 0)
          break;
        parv++;
        if (MyUser(sptr) && opcnt >= MAXMODEPARAMS)
          break;
        /*
         * Check for nickname changes and try to follow these
         * to make sure the right client is affected by the
         * mode change.
         * Even if we find a nick with find_chasing() there
         * is still a reason to ignore in a special case.
         * We need to ignore the mode when:
         * - It is part of a net.burst (from a server and
         *   a MODE_ADD). Ofcourse we don't ignore mode
         *   changes from Uworld.
         * - The found nick is not on the right side off
         *   the net.junction.
         * This fixes the bug that when someone (tries to)
         * ride a net.break and does so with the nick of
         * someone on the otherside, that he is nick collided
         * (killed) but his +o still ops the other person.
         */
        if (MyUser(sptr))
        {
          if (!(who = find_chasing(sptr, parv[0], NULL)))
            break;
        }
        else
        {
          if (!(who = findNUser(parv[0])))
            break;
        }
        if (whatt == MODE_ADD && IsServer(sptr) && who->from != sptr->from &&
            !find_conf_byhost(cptr->confs, sptr->name, CONF_UWORLD))
          break;

        if (!(member_x = find_member_link(chptr, who)) ||
            (MyUser(sptr) && IsZombie(member_x)))
        {
	  send_reply(cptr, ERR_USERNOTINCHANNEL, who->name, chptr->chname);
          break;
        }
        /*
         * if the user is +k, prevent a deop from local user
         */
        if (whatt == MODE_DEL && IsChannelService(who) && *curr == 'o') {
          /*
           * XXX - CHECKME
           */
          if (MyUser(cptr)) {
	    send_reply(cptr, ERR_ISCHANSERVICE, parv[0], chptr->chname);
            break;
           }
           else {
             sprintf_irc(sendbuf,":%s NOTICE * :*** Notice -- Deop of +k user on %s by %s",
                         me.name,chptr->chname,cptr->name);             
           }
        }
#ifdef NO_OPER_DEOP_LCHAN
        /*
         * if the user is an oper on a local channel, prevent him
         * from being deoped. that oper can deop himself though.
         */
        if (whatt == MODE_DEL && IsOperOnLocalChannel(who, chptr->chname) &&
            (who != sptr) && MyUser(cptr) && *curr == 'o')
        {
	  send_reply(cptr, ERR_ISOPERLCHAN, parv[0], chptr->chname);
          break;
        }
#endif
        if (whatt == MODE_ADD)
        {
          lp = &chops[opcnt++];
          lp->value.cptr = who;
          if (IsServer(sptr) && (!(who->flags & FLAGS_TS8) || ((*curr == 'o') &&
              !(member_x->status & (CHFL_SERVOPOK | CHFL_CHANOP)))))
            *badop = ((member_x->status & CHFL_DEOPPED) && (*curr == 'o')) ? 2 : 3;
          lp->flags = (*curr == 'o') ? MODE_CHANOP : MODE_VOICE;
          lp->flags |= MODE_ADD;
        }
        else if (whatt == MODE_DEL)
        {
          lp = &chops[opcnt++];
          lp->value.cptr = who;
          doesdeop = 1;         /* Also when -v */
          lp->flags = (*curr == 'o') ? MODE_CHANOP : MODE_VOICE;
          lp->flags |= MODE_DEL;
        }
        if (*curr == 'o')
          doesop = 1;
        break;
      case 'k':
        if (--parc <= 0)
          break;
        parv++;
        /* check now so we eat the parameter if present */
        if (keychange)
          break;
        else
        {
          char *s = &(*parv)[-1];
          unsigned short count = KEYLEN + 1;

          while (*++s > ' ' && *s != ':' && --count);
          *s = '\0';
          if (!**parv)          /* nothing left in key */
            break;
        }
        if (MyUser(sptr) && opcnt >= MAXMODEPARAMS)
          break;
        if (whatt == MODE_ADD)
        {
          if (*mode->key && !IsServer(cptr))
	    send_reply(cptr, ERR_KEYSET, chptr->chname);
          else if (!*mode->key || IsServer(cptr))
          {
            lp = &chops[opcnt++];
            lp->value.cp = *parv;
            if (strlen(lp->value.cp) > KEYLEN)
              lp->value.cp[KEYLEN] = '\0';
            lp->flags = MODE_KEY | MODE_ADD;
            keychange = 1;
          }
        }
        else if (whatt == MODE_DEL)
        {
          /* Debug((DEBUG_INFO, "removing key: mode->key: >%s< *parv: >%s<", mode->key, *parv)); */
          if (0 == ircd_strcmp(mode->key, *parv) || IsServer(cptr))
          {
            /* Debug((DEBUG_INFO, "key matched")); */
            lp = &chops[opcnt++];
            lp->value.cp = mode->key;
            lp->flags = MODE_KEY | MODE_DEL;
            keychange = 1;
          }
        }
        break;
      case 'b':
        if (--parc <= 0) {
          if (0 == banlsent) {
            /*
             * Only send it once
             */
            send_ban_list(cptr, chptr);
            banlsent = 1;
          }
          break;
        }
        parv++;
        if (EmptyString(*parv))
          break;
        if (MyUser(sptr))
        {
          if ((cp = strchr(*parv, ' ')))
            *cp = 0;
          if (opcnt >= MAXMODEPARAMS || **parv == ':' || **parv == '\0')
            break;
        }
        if (whatt == MODE_ADD)
        {
          lp = &chops[opcnt++];
          lp->value.cp = *parv;
          lp->flags = MODE_ADD | MODE_BAN;
        }
        else if (whatt == MODE_DEL)
        {
          lp = &chops[opcnt++];
          lp->value.cp = *parv;
          lp->flags = MODE_DEL | MODE_BAN;
        }
        break;
      case 'l':
        /*
         * limit 'l' to only *1* change per mode command but
         * eat up others.
         */
        if (limitset)
        {
          if (whatt == MODE_ADD && --parc > 0)
            parv++;
          break;
        }
        if (whatt == MODE_DEL)
        {
          limitset = 1;
          nusers = 0;
          break;
        }
        if (--parc > 0)
        {
          if (EmptyString(*parv))
            break;
          if (MyUser(sptr) && opcnt >= MAXMODEPARAMS)
            break;
          if (!(nusers = atoi(*++parv)))
            continue;
          lp = &chops[opcnt++];
          lp->flags = MODE_ADD | MODE_LIMIT;
          limitset = 1;
          break;
        }
        need_more_params(cptr, "MODE +l");
        break;
      case 'i':         /* falls through for default case */
        if (whatt == MODE_DEL)
          while ((lp = chptr->invites))
            del_invite(lp->value.cptr, chptr);
      default:
        for (ip = flags; *ip; ip += 2)
          if (*(ip + 1) == *curr)
            break;

        if (*ip)
        {
          if (whatt == MODE_ADD)
          {
            if (*ip == MODE_PRIVATE)
              newmode &= ~MODE_SECRET;
            else if (*ip == MODE_SECRET)
              newmode &= ~MODE_PRIVATE;
            newmode |= *ip;
          }
          else
            newmode &= ~*ip;
        }
        else if (!IsServer(cptr))
	  send_reply(cptr, ERR_UNKNOWNMODE, *curr);
        break;
    }
    curr++;
    /*
     * Make sure mode strings such as "+m +t +p +i" are parsed
     * fully.
     */
    if (!*curr && parc > 0)
    {
      curr = *++parv;
      parc--;
      /* If this was from a server, and it is the last
       * parameter and it starts with a digit, it must
       * be the creationtime.  --Run
       */
      if (IsServer(sptr))
      {
        if (parc == 1 && IsDigit(*curr))
        {
          newtime = atoi(curr);
          if (newtime && chptr->creationtime == MAGIC_REMOTE_JOIN_TS)
          {
            chptr->creationtime = newtime;
            *badop = 0;
          }
          gotts = 1;
          if (newtime == 0)
          {
            *badop = 2;
            hacknotice = 1;
          }
          else if (newtime > chptr->creationtime)
          {                     /* It is a net-break ride if we have ops.
                                   bounce modes if we have ops.  --Run */
            if (doesdeop)
              *badop = 2;
            else if (chptr->creationtime == 0)
            {
              if (chptr->creationtime == 0 || doesop)
                chptr->creationtime = newtime;
              *badop = 0;
            }
            /* Bounce: */
            else
              *badop = 1;
          }
          /*
           * A legal *badop can occur when two
           * people join simultaneously a channel,
           * Allow for 10 min of lag (and thus hacking
           * on channels younger then 10 min) --Run
           */
          else if (*badop == 0 ||
              chptr->creationtime > (TStime() - TS_LAG_TIME))
          {
            if (newtime < chptr->creationtime)
              chptr->creationtime = newtime;
            *badop = 0;
          }
          break;
        }
      }
      else
        *badop = 0;
    }
  }                             /* end of while loop for MODE processing */

#ifdef OPER_MODE_LCHAN
  /*
   * Now reject non chan ops. Accept modes from opers on local channels
   * even if they are deopped
   */
  if (!IsServer(cptr) &&
      (!member_y || !(IsChanOp(member_y) ||
                 IsOperOnLocalChannel(sptr, chptr->chname))))
#else
  if (!IsServer(cptr) && (!member_y || !IsChanOp(member_y)))
#endif
  {
    *badop = 0;
    return (opcnt || newmode != mode->mode || limitset || keychange) ? 0 : -1;
  }

  if (doesop && newtime == 0 && IsServer(sptr))
    *badop = 2;

  if (*badop >= 2 &&
      (aconf = find_conf_byhost(cptr->confs, sptr->name, CONF_UWORLD)))
    *badop = 4;

#ifdef OPER_MODE_LCHAN
  bounce = (*badop == 1 || *badop == 2 ||
            (is_deopped(sptr, chptr) &&
             !IsOperOnLocalChannel(sptr, chptr->chname))) ? 1 : 0;
#else
  bounce = (*badop == 1 || *badop == 2 || is_deopped(sptr, chptr)) ? 1 : 0;
#endif

  whatt = 0;
  for (ip = flags; *ip; ip += 2) {
    if ((*ip & newmode) && !(*ip & oldm.mode))
    {
      if (bounce)
      {
        if (bwhatt != MODE_DEL)
        {
          *bmbuf++ = '-';
          bwhatt = MODE_DEL;
        }
        *bmbuf++ = *(ip + 1);
      }
      else
      {
        if (whatt != MODE_ADD)
        {
          *mbuf++ = '+';
          whatt = MODE_ADD;
        }
        mode->mode |= *ip;
        *mbuf++ = *(ip + 1);
      }
    }
  }
  for (ip = flags; *ip; ip += 2) {
    if ((*ip & oldm.mode) && !(*ip & newmode))
    {
      if (bounce)
      {
        if (bwhatt != MODE_ADD)
        {
          *bmbuf++ = '+';
          bwhatt = MODE_ADD;
        }
        *bmbuf++ = *(ip + 1);
      }
      else
      {
        if (whatt != MODE_DEL)
        {
          *mbuf++ = '-';
          whatt = MODE_DEL;
        }
        mode->mode &= ~*ip;
        *mbuf++ = *(ip + 1);
      }
    }
  }
  blen = nblen = 0;
  if (limitset && !nusers && mode->limit)
  {
    if (bounce)
    {
      if (bwhatt != MODE_ADD)
      {
        *bmbuf++ = '+';
        bwhatt = MODE_ADD;
      }
      *bmbuf++ = 'l';
      sprintf(numeric, "%-15d", mode->limit);
      if ((cp = strchr(numeric, ' ')))
        *cp = '\0';
      strcat(bpbuf, numeric);
      blen += strlen(numeric);
      strcat(bpbuf, " ");
      strcat(nbpbuf, numeric);
      nblen += strlen(numeric);
      strcat(nbpbuf, " ");
    }
    else
    {
      if (whatt != MODE_DEL)
      {
        *mbuf++ = '-';
        whatt = MODE_DEL;
      }
      mode->mode &= ~MODE_LIMIT;
      mode->limit = 0;
      *mbuf++ = 'l';
    }
  }
  /*
   * Reconstruct "+bkov" chain.
   */
  if (opcnt)
  {
    int i = 0;
    char c = 0;
    unsigned int prev_whatt = 0;

    for (; i < opcnt; i++)
    {
      lp = &chops[i];
      /*
       * make sure we have correct mode change sign
       */
      if (whatt != (lp->flags & (MODE_ADD | MODE_DEL)))
      {
        if (lp->flags & MODE_ADD)
        {
          *mbuf++ = '+';
          prev_whatt = whatt;
          whatt = MODE_ADD;
        }
        else
        {
          *mbuf++ = '-';
          prev_whatt = whatt;
          whatt = MODE_DEL;
        }
      }
      len = strlen(pbuf);
      nlen = strlen(npbuf);
      /*
       * get c as the mode char and tmp as a pointer to
       * the parameter for this mode change.
       */
      switch (lp->flags & MODE_WPARAS)
      {
        case MODE_CHANOP:
          c = 'o';
          cp = lp->value.cptr->name;
          break;
        case MODE_VOICE:
          c = 'v';
          cp = lp->value.cptr->name;
          break;
        case MODE_BAN:
          /*
           * I made this a bit more user-friendly (tm):
           * nick = nick!*@*
           * nick!user = nick!user@*
           * user@host = *!user@host
           * host.name = *!*@host.name    --Run
           */
          c = 'b';
          cp = pretty_mask(lp->value.cp);
          break;
        case MODE_KEY:
          c = 'k';
          cp = lp->value.cp;
          break;
        case MODE_LIMIT:
          c = 'l';
          sprintf(numeric, "%-15d", nusers);
          if ((cp = strchr(numeric, ' ')))
            *cp = '\0';
          cp = numeric;
          break;
      }

      /* What could be added: cp+' '+' '+<TS>+'\0' */
      if (len + strlen(cp) + 13 > MODEBUFLEN ||
          nlen + strlen(cp) + NUMNICKLEN + 12 > MODEBUFLEN)
        break;

      switch (lp->flags & MODE_WPARAS)
      {
        case MODE_KEY:
          if (strlen(cp) > KEYLEN)
            *(cp + KEYLEN) = '\0';
          if ((whatt == MODE_ADD && (*mode->key == '\0' ||
               0 != ircd_strcmp(mode->key, cp))) ||
              (whatt == MODE_DEL && (*mode->key != '\0')))
          {
            if (bounce)
            {
              if (*mode->key == '\0')
              {
                if (bwhatt != MODE_DEL)
                {
                  *bmbuf++ = '-';
                  bwhatt = MODE_DEL;
                }
                strcat(bpbuf, cp);
                blen += strlen(cp);
                strcat(bpbuf, " ");
                blen++;
                strcat(nbpbuf, cp);
                nblen += strlen(cp);
                strcat(nbpbuf, " ");
                nblen++;
              }
              else
              {
                if (bwhatt != MODE_ADD)
                {
                  *bmbuf++ = '+';
                  bwhatt = MODE_ADD;
                }
                strcat(bpbuf, mode->key);
                blen += strlen(mode->key);
                strcat(bpbuf, " ");
                blen++;
                strcat(nbpbuf, mode->key);
                nblen += strlen(mode->key);
                strcat(nbpbuf, " ");
                nblen++;
              }
              *bmbuf++ = c;
              mbuf--;
              if (*mbuf != '+' && *mbuf != '-')
                mbuf++;
              else
                whatt = prev_whatt;
            }
            else
            {
              *mbuf++ = c;
              strcat(pbuf, cp);
              len += strlen(cp);
              strcat(pbuf, " ");
              len++;
              strcat(npbuf, cp);
              nlen += strlen(cp);
              strcat(npbuf, " ");
              nlen++;
              if (whatt == MODE_ADD)
                ircd_strncpy(mode->key, cp, KEYLEN);
              else
                *mode->key = '\0';
            }
          }
          break;
        case MODE_LIMIT:
          if (nusers && nusers != mode->limit)
          {
            if (bounce)
            {
              if (mode->limit == 0)
              {
                if (bwhatt != MODE_DEL)
                {
                  *bmbuf++ = '-';
                  bwhatt = MODE_DEL;
                }
              }
              else
              {
                if (bwhatt != MODE_ADD)
                {
                  *bmbuf++ = '+';
                  bwhatt = MODE_ADD;
                }
                sprintf(numeric, "%-15d", mode->limit);
                if ((cp = strchr(numeric, ' ')))
                  *cp = '\0';
                strcat(bpbuf, numeric);
                blen += strlen(numeric);
                strcat(bpbuf, " ");
                blen++;
                strcat(nbpbuf, numeric);
                nblen += strlen(numeric);
                strcat(nbpbuf, " ");
                nblen++;
              }
              *bmbuf++ = c;
              mbuf--;
              if (*mbuf != '+' && *mbuf != '-')
                mbuf++;
              else
                whatt = prev_whatt;
            }
            else
            {
              *mbuf++ = c;
              strcat(pbuf, cp);
              len += strlen(cp);
              strcat(pbuf, " ");
              len++;
              strcat(npbuf, cp);
              nlen += strlen(cp);
              strcat(npbuf, " ");
              nlen++;
              mode->limit = nusers;
            }
          }
          break;
        case MODE_CHANOP:
        case MODE_VOICE:
          member_y = find_member_link(chptr, lp->value.cptr);
          if (lp->flags & MODE_ADD)
          {
            change = (~member_y->status) & CHFL_VOICED_OR_OPPED & lp->flags;
            if (change && bounce)
            {
              if (lp->flags & MODE_CHANOP)
                SetDeopped(member_y);

              if (bwhatt != MODE_DEL)
              {
                *bmbuf++ = '-';
                bwhatt = MODE_DEL;
              }
              *bmbuf++ = c;
              strcat(bpbuf, lp->value.cptr->name);
              blen += strlen(lp->value.cptr->name);
              strcat(bpbuf, " ");
              blen++;
              sprintf_irc(nbpbuf + nblen, "%s%s ", NumNick(lp->value.cptr));
              nblen += strlen(nbpbuf + nblen);
              change = 0;
            }
            else if (change)
            {
              member_y->status |= lp->flags & CHFL_VOICED_OR_OPPED;
              if (IsChanOp(member_y))
              {
                ClearDeopped(member_y);
                if (IsServer(sptr))
                  ClearServOpOk(member_y);
              }
            }
          }
          else
          {
            change = member_y->status & CHFL_VOICED_OR_OPPED & lp->flags;
            if (change && bounce)
            {
              if (lp->flags & MODE_CHANOP)
                ClearDeopped(member_y);
              if (bwhatt != MODE_ADD)
              {
                *bmbuf++ = '+';
                bwhatt = MODE_ADD;
              }
              *bmbuf++ = c;
              strcat(bpbuf, lp->value.cptr->name);
              blen += strlen(lp->value.cptr->name);
              strcat(bpbuf, " ");
              blen++;
              sprintf_irc(nbpbuf + nblen, "%s%s ", NumNick(lp->value.cptr));
              blen += strlen(bpbuf + blen);
              change = 0;
            }
            else
            {
              member_y->status &= ~change;
              if ((change & MODE_CHANOP) && IsServer(sptr))
                SetDeopped(member_y);
            }
          }
          if (change || *badop == 2 || *badop == 4)
          {
            *mbuf++ = c;
            strcat(pbuf, cp);
            len += strlen(cp);
            strcat(pbuf, " ");
            len++;
            sprintf_irc(npbuf + nlen, "%s%s ", NumNick(lp->value.cptr));
            nlen += strlen(npbuf + nlen);
            npbuf[nlen++] = ' ';
            npbuf[nlen] = 0;
          }
          else
          {
            mbuf--;
            if (*mbuf != '+' && *mbuf != '-')
              mbuf++;
            else
              whatt = prev_whatt;
          }
          break;
        case MODE_BAN:
/*
 * Only bans aren't bounced, it makes no sense to bounce last second
 * bans while propagating bans done before the net.rejoin. The reason
 * why I don't bounce net.rejoin bans is because it is too much
 * work to take care of too long strings adding the necessary TS to
 * net.burst bans -- RunLazy
 * We do have to check for *badop==2 now, we don't want HACKs to take
 * effect.
 *
 * Since BURST - I *did* implement net.rejoin ban bouncing. So now it
 * certainly makes sense to also bounce 'last second' bans (bans done
 * after the net.junction). -- RunHardWorker
 */
          if ((change = (whatt & MODE_ADD) &&
              !add_banid(sptr, chptr, cp, !bounce, !add_banid_called)))
            add_banid_called = 1;
          else
            change = (whatt & MODE_DEL) && !del_banid(chptr, cp, !bounce);

          if (bounce && change)
          {
            change = 0;
            if ((whatt & MODE_ADD))
            {
              if (bwhatt != MODE_DEL)
              {
                *bmbuf++ = '-';
                bwhatt = MODE_DEL;
              }
            }
            else if ((whatt & MODE_DEL))
            {
              if (bwhatt != MODE_ADD)
              {
                *bmbuf++ = '+';
                bwhatt = MODE_ADD;
              }
            }
            *bmbuf++ = c;
            strcat(bpbuf, cp);
            blen += strlen(cp);
            strcat(bpbuf, " ");
            blen++;
            strcat(nbpbuf, cp);
            nblen += strlen(cp);
            strcat(nbpbuf, " ");
            nblen++;
          }
          if (change)
          {
            *mbuf++ = c;
            strcat(pbuf, cp);
            len += strlen(cp);
            strcat(pbuf, " ");
            len++;
            strcat(npbuf, cp);
            nlen += strlen(cp);
            strcat(npbuf, " ");
            nlen++;
          }
          else
          {
            mbuf--;
            if (*mbuf != '+' && *mbuf != '-')
              mbuf++;
            else
              whatt = prev_whatt;
          }
          break;
      }
    }                           /* for (; i < opcnt; i++) */
  }                             /* if (opcnt) */

  *mbuf++ = '\0';
  *bmbuf++ = '\0';

  /* Bounce here */
  if (!hacknotice && *bmodebuf && chptr->creationtime)
  {
    sendcmdto_one(&me, CMD_MODE, cptr, "%H %s %s %Tu", chptr, bmodebuf,
		  nbparambuf, *badop == 2 ? (time_t) 0 : chptr->creationtime);
  }
  /* If there are possibly bans to re-add, bounce them now */
  if (add_banid_called && bounce)
  {
    struct SLink *ban[6];               /* Max 6 bans at a time */
    size_t len[6], sblen, total_len;
    int cnt, delayed = 0;
    while (delayed || (ban[0] = next_overlapped_ban()))
    {
      len[0] = strlen(ban[0]->value.ban.banstr);
      cnt = 1;                  /* We already got one ban :) */
      /* XXX sendbuf used to send ban bounces! */
      sblen = sprintf_irc(sendbuf, ":%s MODE %s +b",
          me.name, chptr->chname) - sendbuf;
      total_len = sblen + 1 + len[0];   /* 1 = ' ' */
      /* Find more bans: */
      delayed = 0;
      while (cnt < 6 && (ban[cnt] = next_overlapped_ban()))
      {
        len[cnt] = strlen(ban[cnt]->value.ban.banstr);
        if (total_len + 5 + len[cnt] > BUFSIZE) /* 5 = "b \r\n\0" */
        {
          delayed = cnt + 1;    /* != 0 */
          break;                /* Flush */
        }
        sendbuf[sblen++] = 'b';
        total_len += 2 + len[cnt++];    /* 2 = "b " */
      }
      while (cnt--)
      {
        sendbuf[sblen++] = ' ';
        strcpy(sendbuf + sblen, ban[cnt]->value.ban.banstr);
        sblen += len[cnt];
      }
      sendbufto_one(cptr);      /* Send bounce to uplink */
      if (delayed)
        ban[0] = ban[delayed - 1];
    }
  }
  /* Send -b's of overlapped bans to clients to keep them synchronized */
  if (add_banid_called && !bounce)
  {
    struct SLink *ban;
    char *banstr[6];            /* Max 6 bans at a time */
    size_t len[6], sblen, psblen, total_len;
    int cnt, delayed = 0;
    struct Membership* member_z;
    struct Client *acptr;
    if (IsServer(sptr))
      /* XXX sendbuf used to send ban bounces! */
      psblen = sprintf_irc(sendbuf, ":%s MODE %s -b",
          sptr->name, chptr->chname) - sendbuf;
    else                        /* We rely on IsRegistered(sptr) being true for MODE */
      psblen = sprintf_irc(sendbuf, ":%s!%s@%s MODE %s -b", sptr->name,
          sptr->user->username, sptr->user->host, chptr->chname) - sendbuf;
    while (delayed || (ban = next_removed_overlapped_ban()))
    {
      if (!delayed)
      {
        len[0] = strlen((banstr[0] = ban->value.ban.banstr));
        ban->value.ban.banstr = NULL;
      }
      cnt = 1;                  /* We already got one ban :) */
      sblen = psblen;
      total_len = sblen + 1 + len[0];   /* 1 = ' ' */
      /* Find more bans: */
      delayed = 0;
      while (cnt < 6 && (ban = next_removed_overlapped_ban()))
      {
        len[cnt] = strlen((banstr[cnt] = ban->value.ban.banstr));
        ban->value.ban.banstr = NULL;
        if (total_len + 5 + len[cnt] > BUFSIZE) /* 5 = "b \r\n\0" */
        {
          delayed = cnt + 1;    /* != 0 */
          break;                /* Flush */
        }
        sendbuf[sblen++] = 'b';
        total_len += 2 + len[cnt++];    /* 2 = "b " */
      }
      while (cnt--)
      {
        sendbuf[sblen++] = ' ';
        strcpy(sendbuf + sblen, banstr[cnt]);
        MyFree(banstr[cnt]);
        sblen += len[cnt];
      }
      for (member_z = chptr->members; member_z; member_z = member_z->next_member) {
        acptr = member_z->user;
        if (MyConnect(acptr) && !IsZombie(member_z))
          sendbufto_one(acptr);
      }
      if (delayed)
      {
        banstr[0] = banstr[delayed - 1];
        len[0] = len[delayed - 1];
      }
    }
  }

  return gotts ? 1 : -1;
}

/* We are now treating the <key> part of /join <channel list> <key> as a key
 * ring; that is, we try one key against the actual channel key, and if that
 * doesn't work, we try the next one, and so on. -Kev -Texaco
 * Returns: 0 on match, 1 otherwise
 * This version contributed by SeKs <intru@info.polymtl.ca>
 */
static int compall(char *key, char *keyring)
{
  char *p1;

top:
  p1 = key;                     /* point to the key... */
  while (*p1 && *p1 == *keyring)
  {                             /* step through the key and ring until they
                                   don't match... */
    p1++;
    keyring++;
  }

  if (!*p1 && (!*keyring || *keyring == ','))
    /* ok, if we're at the end of the and also at the end of one of the keys
       in the keyring, we have a match */
    return 0;

  if (!*keyring)                /* if we're at the end of the key ring, there
                                   weren't any matches, so we return 1 */
    return 1;

  /* Not at the end of the key ring, so step
     through to the next key in the ring: */
  while (*keyring && *(keyring++) != ',');

  goto top;                     /* and check it against the key */
}

int can_join(struct Client *sptr, struct Channel *chptr, char *key)
{
  struct SLink *lp;
  int overrideJoin = 0;  
  
  /*
   * Now a banned user CAN join if invited -- Nemesi
   * Now a user CAN escape channel limit if invited -- bfriendly
   * Now a user CAN escape anything if invited -- Isomer
   */

  for (lp = sptr->user->invited; lp; lp = lp->next)
    if (lp->value.chptr == chptr)
      return 0;
  
#ifdef OPER_WALK_THROUGH_LMODES
  /* An oper can force a join on a local channel using "OVERRIDE" as the key. 
     a HACK(4) notice will be sent if he would not have been supposed
     to join normally. */ 
  if (IsOperOnLocalChannel(sptr,chptr->chname) && !BadPtr(key) && compall("OVERRIDE",key) == 0)
  {
    overrideJoin = MAGIC_OPER_OVERRIDE;
  }
#endif

  if (chptr->mode.mode & MODE_INVITEONLY)
  	return overrideJoin + ERR_INVITEONLYCHAN;
  	
  if (chptr->mode.limit && chptr->users >= chptr->mode.limit)
  	return overrideJoin + ERR_CHANNELISFULL;
  	
  if (is_banned(sptr, chptr, NULL))
  	return overrideJoin + ERR_BANNEDFROMCHAN;
  
  /*
   * now using compall (above) to test against a whole key ring -Kev
   */
  if (*chptr->mode.key && (EmptyString(key) || compall(chptr->mode.key, key)))
    return overrideJoin + ERR_BADCHANNELKEY;

  if (overrideJoin) 	
  	return ERR_DONTCHEAT;
  	
  return 0;
}

/*
 * Remove bells and commas from channel name
 */
void clean_channelname(char *cn)
{
  int i;

  for (i = 0; cn[i]; i++) {
    if (i >= CHANNELLEN || !IsChannelChar(cn[i])) {
      cn[i] = '\0';
      return;
    }
    if (IsChannelLower(cn[i])) {
      cn[i] = ToLower(cn[i]);
#ifndef FIXME
      /*
       * Remove for .08+
       * toupper(0xd0)
       */
      if ((unsigned char)(cn[i]) == 0xd0)
        cn[i] = (char) 0xf0;
#endif
    }
  }
}

/*
 *  Get Channel block for i (and allocate a new channel
 *  block, if it didn't exists before).
 */
struct Channel *get_channel(struct Client *cptr, char *chname, ChannelGetType flag)
{
  struct Channel *chptr;
  int len;

  if (EmptyString(chname))
    return NULL;

  len = strlen(chname);
  if (MyUser(cptr) && len > CHANNELLEN)
  {
    len = CHANNELLEN;
    *(chname + CHANNELLEN) = '\0';
  }
  if ((chptr = FindChannel(chname)))
    return (chptr);
  if (flag == CGT_CREATE)
  {
    chptr = (struct Channel*) MyMalloc(sizeof(struct Channel) + len);
    assert(0 != chptr);
    ++UserStats.channels;
    memset(chptr, 0, sizeof(struct Channel));
    strcpy(chptr->chname, chname);
    if (GlobalChannelList)
      GlobalChannelList->prev = chptr;
    chptr->prev = NULL;
    chptr->next = GlobalChannelList;
    chptr->creationtime = MyUser(cptr) ? TStime() : (time_t) 0;
    GlobalChannelList = chptr;
    hAddChannel(chptr);
  }
  return chptr;
}

void add_invite(struct Client *cptr, struct Channel *chptr)
{
  struct SLink *inv, **tmp;

  del_invite(cptr, chptr);
  /*
   * Delete last link in chain if the list is max length
   */
  assert(list_length(cptr->user->invited) == cptr->user->invites);
  if (cptr->user->invites>=MAXCHANNELSPERUSER)
    del_invite(cptr, cptr->user->invited->value.chptr);
  /*
   * Add client to channel invite list
   */
  inv = make_link();
  inv->value.cptr = cptr;
  inv->next = chptr->invites;
  chptr->invites = inv;
  /*
   * Add channel to the end of the client invite list
   */
  for (tmp = &(cptr->user->invited); *tmp; tmp = &((*tmp)->next));
  inv = make_link();
  inv->value.chptr = chptr;
  inv->next = NULL;
  (*tmp) = inv;
  cptr->user->invites++;
}

/*
 * Delete Invite block from channel invite list and client invite list
 */
void del_invite(struct Client *cptr, struct Channel *chptr)
{
  struct SLink **inv, *tmp;

  for (inv = &(chptr->invites); (tmp = *inv); inv = &tmp->next)
    if (tmp->value.cptr == cptr)
    {
      *inv = tmp->next;
      free_link(tmp);
      tmp = 0;
      cptr->user->invites--;
      break;
    }

  for (inv = &(cptr->user->invited); (tmp = *inv); inv = &tmp->next)
    if (tmp->value.chptr == chptr)
    {
      *inv = tmp->next;
      free_link(tmp);
      tmp = 0;
      break;
    }
}

/* List and skip all channels that are listen */
void list_next_channels(struct Client *cptr, int nr)
{
  struct ListingArgs *args = cptr->listing;
  struct Channel *chptr = args->chptr;
  chptr->mode.mode &= ~MODE_LISTED;
  while (is_listed(chptr) || --nr >= 0)
  {
    for (; chptr; chptr = chptr->next)
    {
      if (!cptr->user || (SecretChannel(chptr) && !find_channel_member(cptr, chptr)))
        continue;
      if (chptr->users > args->min_users && chptr->users < args->max_users &&
          chptr->creationtime > args->min_time &&
          chptr->creationtime < args->max_time &&
          (!args->topic_limits || (*chptr->topic &&
          chptr->topic_time > args->min_topic_time &&
          chptr->topic_time < args->max_topic_time)))
      {
        if (ShowChannel(cptr,chptr))
	  send_reply(cptr, RPL_LIST, chptr->chname, chptr->users,
		     chptr->topic);
        chptr = chptr->next;
        break;
      }
    }
    if (!chptr)
    {
      MyFree(cptr->listing);
      cptr->listing = NULL;
      send_reply(cptr, RPL_LISTEND);
      break;
    }
  }
  if (chptr)
  {
    cptr->listing->chptr = chptr;
    chptr->mode.mode |= MODE_LISTED;
  }
}

/* XXX AIEEEE! sendbuf is an institution here :( */
void add_token_to_sendbuf(char *token, size_t *sblenp, int *firstp,
    int *send_itp, char is_a_ban, int mode)
{
  int first = *firstp;

  /*
   * Heh - we do not need to test if it still fits in the buffer, because
   * this BURST message is reconstructed from another BURST message, and
   * it only can become smaller. --Run
   */

  if (*firstp)                  /* First token in this parameter ? */
  {
    *firstp = 0;
    if (*send_itp == 0)
      *send_itp = 1;            /* Buffer contains data to be sent */
    sendbuf[(*sblenp)++] = ' ';
    if (is_a_ban)
    {
      sendbuf[(*sblenp)++] = ':';       /* Bans are always the last "parv" */
      sendbuf[(*sblenp)++] = is_a_ban;
    }
  }
  else                          /* Of course, 'send_it' is already set here */
    /* Seperate banmasks with a space because
       they can contain commas themselfs: */
    sendbuf[(*sblenp)++] = is_a_ban ? ' ' : ',';
  strcpy(sendbuf + *sblenp, token);
  *sblenp += strlen(token);
  if (!is_a_ban)                /* nick list ? Need to take care
                                   of modes for nicks: */
  {
    static int last_mode = 0;
    mode &= CHFL_CHANOP | CHFL_VOICE;
    if (first)
      last_mode = 0;
    if (last_mode != mode)      /* Append mode like ':ov' if changed */
    {
      last_mode = mode;
      sendbuf[(*sblenp)++] = ':';
      if (mode & CHFL_CHANOP)
        sendbuf[(*sblenp)++] = 'o';
      if (mode & CHFL_VOICE)
        sendbuf[(*sblenp)++] = 'v';
    }
    sendbuf[*sblenp] = '\0';
  }
}

void cancel_mode(struct Client *sptr, struct Channel *chptr, char m,
                        const char *param, int *count)
{
  static char* pb;
  static char* sbp;
  static char* sbpi;
  int          paramdoesntfit = 0;
  char parabuf[MODEBUFLEN];

  assert(0 != sptr);
  assert(0 != chptr);
  assert(0 != count);
  
  if (*count == -1)             /* initialize ? */
  {
    /* XXX sendbuf used! */
    sbp = sbpi =
        sprintf_irc(sendbuf, ":%s MODE %s -", sptr->name, chptr->chname);
    pb = parabuf;
    *count = 0;
  }
  /* m == 0 means flush */
  if (m)
  {
    if (param)
    {
      size_t nplen = strlen(param);
      if (pb - parabuf + nplen + 23 > MODEBUFLEN)
        paramdoesntfit = 1;
      else
      {
        *sbp++ = m;
        *pb++ = ' ';
        strcpy(pb, param);
        pb += nplen;
        ++*count;
      }
    }
    else
      *sbp++ = m;
  }
  else if (*count == 0)
    return;
  if (*count == 6 || !m || paramdoesntfit)
  {
    struct Membership* member;
    strcpy(sbp, parabuf);
    for (member = chptr->members; member; member = member->next_member)
      if (MyUser(member->user))
        sendbufto_one(member->user);
    sbp = sbpi;
    pb = parabuf;
    *count = 0;
  }
  if (paramdoesntfit)
  {
    *sbp++ = m;
    *pb++ = ' ';
    strcpy(pb, param);
    pb += strlen(param);
    ++*count;
  }
}


/*
 * Consider:
 *
 *                     client
 *                       |
 *                       c
 *                       |
 *     X --a--> A --b--> B --d--> D
 *                       |
 *                      who
 *
 * Where `who' is being KICK-ed by a "KICK" message received by server 'A'
 * via 'a', or on server 'B' via either 'b' or 'c', or on server D via 'd'.
 *
 * a) On server A : set CHFL_ZOMBIE for `who' (lp) and pass on the KICK.
 *    Remove the user immedeately when no users are left on the channel.
 * b) On server B : remove the user (who/lp) from the channel, send a
 *    PART upstream (to A) and pass on the KICK.
 * c) KICKed by `client'; On server B : remove the user (who/lp) from the
 *    channel, and pass on the KICK.
 * d) On server D : remove the user (who/lp) from the channel, and pass on
 *    the KICK.
 *
 * Note:
 * - Setting the ZOMBIE flag never hurts, we either remove the
 *   client after that or we don't.
 * - The KICK message was already passed on, as should be in all cases.
 * - `who' is removed in all cases except case a) when users are left.
 * - A PART is only sent upstream in case b).
 *
 * 2 aug 97:
 *
 *              6
 *              |
 *  1 --- 2 --- 3 --- 4 --- 5
 *        |           |
 *      kicker       who
 *
 * We also need to turn 'who' into a zombie on servers 1 and 6,
 * because a KICK from 'who' (kicking someone else in that direction)
 * can arrive there afterwards - which should not be bounced itself.
 * Therefore case a) also applies for servers 1 and 6.
 *
 * --Run
 */
void make_zombie(struct Membership* member, struct Client* who, struct Client* cptr,
                 struct Client* sptr, struct Channel* chptr)
{
  assert(0 != member);
  assert(0 != who);
  assert(0 != cptr);
  assert(0 != chptr);

  /* Default for case a): */
  SetZombie(member);

  /* Case b) or c) ?: */
  if (MyUser(who))      /* server 4 */
  {
    if (IsServer(cptr)) /* Case b) ? */
      sendcmdto_one(who, CMD_PART, cptr, "%H", chptr);
    remove_user_from_channel(who, chptr);
    return;
  }
  if (who->from == cptr)        /* True on servers 1, 5 and 6 */
  {
    struct Client *acptr = IsServer(sptr) ? sptr : sptr->user->server;
    for (; acptr != &me; acptr = acptr->serv->up)
      if (acptr == who->user->server)   /* Case d) (server 5) */
      {
        remove_user_from_channel(who, chptr);
        return;
      }
  }

  /* Case a) (servers 1, 2, 3 and 6) */
  if (channel_all_zombies(chptr))
    remove_user_from_channel(who, chptr);

  Debug((DEBUG_INFO, "%s is now a zombie on %s", who->name, chptr->chname));
}

int number_of_zombies(struct Channel *chptr)
{
  struct Membership* member;
  int                count = 0;

  assert(0 != chptr);
  for (member = chptr->members; member; member = member->next_member) {
    if (IsZombie(member))
      ++count;
  }
  return count;
}

/* XXX we can probably get rid of send_user_joins */
void send_user_joins(struct Client *cptr, struct Client *user)
{
  struct Membership* chan;
  struct Channel*    chptr;
  int   cnt = 0;
  int   len = 0;
  int   clen;
  char* mask;
  char  buf[BUFSIZE];

  *buf = ':';
  strcpy(buf + 1, user->name);
  strcat(buf, " JOIN ");
  len = strlen(user->name) + 7;

  for (chan = user->user->channel; chan; chan = chan->next_channel)
  {
    chptr = chan->channel;
    assert(0 != chptr);

    if ((mask = strchr(chptr->chname, ':')))
      if (match(++mask, cptr->name))
        continue;
    if (*chptr->chname == '&')
      continue;
    if (IsZombie(chan))
      continue;
    clen = strlen(chptr->chname);
    if (clen + 1 + len > BUFSIZE - 3)
    {
      if (cnt)
      {
        buf[len - 1] = '\0';
        sendto_one(cptr, "%s", buf); /* XXX Possibly DEAD */
      }
      *buf = ':';
      strcpy(buf + 1, user->name);
      strcat(buf, " JOIN ");
      len = strlen(user->name) + 7;
      cnt = 0;
    }
    strcpy(buf + len, chptr->chname);
    cnt++;
    len += clen;
    if (chan->next_channel)
    {
      len++;
      strcat(buf, ",");
    }
  }
  if (*buf && cnt)
    sendto_one(cptr, "%s", buf); /* XXX Possibly DEAD */
}

/*
 * send_hack_notice()
 *
 * parc & parv[] are the same as that of the calling function:
 *   mtype == 1 is from m_mode, 2 is from m_create, 3 is from m_kick.
 *
 * This function prepares sendbuf with the server notices and wallops
 *   to be sent for all hacks.  -Ghostwolf 18-May-97
 */
/* XXX let's get rid of this if we can */
void send_hack_notice(struct Client *cptr, struct Client *sptr, int parc,
                      char *parv[], int badop, int mtype)
{
  struct Channel *chptr;
  static char params[MODEBUFLEN];
  int i = 3;
  chptr = FindChannel(parv[1]);
  *params = '\0';

  /* P10 servers require numeric nick conversion before sending. */
  switch (mtype)
  {
    case 1:                     /* Convert nicks for MODE HACKs here  */
    {
      char *mode = parv[2];
      while (i < parc)
      {
        while (*mode && *mode != 'o' && *mode != 'v')
          ++mode;
        strcat(params, " ");
        if (*mode == 'o' || *mode == 'v')
        {
          /*
           * blindly stumble through parameter list hoping one of them
           * might turn out to be a numeric nick
           * NOTE: this should not cause a problem but _may_ end up finding
           * something we aren't looking for. findNUser should be able to
           * handle any garbage that is thrown at it, but may return a client
           * if we happen to get lucky with a mode string or a timestamp
           */
          struct Client *acptr;
          if ((acptr = findNUser(parv[i])) != NULL)     /* Convert nicks here */
            strcat(params, acptr->name);
          else
          {
            strcat(params, "<");
            strcat(params, parv[i]);
            strcat(params, ">");
          }
        }
        else                    /* If it isn't a numnick, send it 'as is' */
          strcat(params, parv[i]);
        i++;
      }
      sprintf_irc(sendbuf,
          ":%s NOTICE * :*** Notice -- %sHACK(%d): %s MODE %s %s%s ["
          TIME_T_FMT "]", me.name, (badop == 3) ? "BOUNCE or " : "", badop,
          parv[0], parv[1], parv[2], params, chptr->creationtime);
      sendbufto_op_mask((badop == 3) ? SNO_HACK3 : (badop == /* XXX DYING */
          4) ? SNO_HACK4 : SNO_HACK2);

      if ((IsServer(sptr)) && (badop == 2))
      {
        sprintf_irc(sendbuf, ":%s DESYNCH :HACK: %s MODE %s %s%s",
            me.name, parv[0], parv[1], parv[2], params);
        sendbufto_serv_butone(cptr); /* XXX DYING */
      }
      break;
    }
    case 2:                     /* No conversion is needed for CREATE; the only numnick is sptr */
    {
      sendto_serv_butone(cptr, ":%s DESYNCH :HACK: %s CREATE %s %s", /* XXX DYING */
          me.name, sptr->name, chptr->chname, parv[2]);
      sendto_op_mask(SNO_HACK2, "HACK(2): %s CREATE %s %s", /* XXX DYING */
          sptr->name, chptr->chname, parv[2]);
      break;
    }
    case 3:                     /* Convert nick in KICK message */
    {
      struct Client *acptr;
      if ((acptr = findNUser(parv[2])) != NULL) /* attempt to convert nick */
        sprintf_irc(sendbuf,
            ":%s NOTICE * :*** Notice -- HACK: %s KICK %s %s :%s",
            me.name, sptr->name, parv[1], acptr->name, parv[3]);
      else                      /* if conversion fails, send it 'as is' in <>'s */
        sprintf_irc(sendbuf,
            ":%s NOTICE * :*** Notice -- HACK: %s KICK %s <%s> :%s",
            me.name, sptr->name, parv[1], parv[2], parv[3]);
      sendbufto_op_mask(SNO_HACK4); /* XXX DYING */
      break;
    }
  }
}

/*
 * This helper function builds an argument string in strptr, consisting
 * of the original string, a space, and str1 and str2 concatenated (if,
 * of course, str2 is not NULL)
 */
static void
build_string(char *strptr, int *strptr_i, char *str1, char *str2, char c)
{
  if (c)
    strptr[(*strptr_i)++] = c;

  while (*str1)
    strptr[(*strptr_i)++] = *(str1++);

  if (str2)
    while (*str2)
      strptr[(*strptr_i)++] = *(str2++);

  strptr[(*strptr_i)] = '\0';
}

/*
 * This is the workhorse of our ModeBuf suite; this actually generates the
 * output MODE commands, HACK notices, or whatever.  It's pretty complicated.
 */
static int
modebuf_flush_int(struct ModeBuf *mbuf, int all)
{
  /* we only need the flags that don't take args right now */
  static int flags[] = {
/*  MODE_CHANOP,	'o', */
/*  MODE_VOICE,		'v', */
    MODE_PRIVATE,	'p',
    MODE_SECRET,	's',
    MODE_MODERATED,	'm',
    MODE_TOPICLIMIT,	't',
    MODE_INVITEONLY,	'i',
    MODE_NOPRIVMSGS,	'n',
/*  MODE_KEY,		'k', */
/*  MODE_BAN,		'b', */
/*  MODE_LIMIT,		'l', */
    0x0, 0x0
  };
  int i;
  int *flag_p;

  struct Client *app_source; /* where the MODE appears to come from */

  char addbuf[20]; /* accumulates +psmtin, etc. */
  int addbuf_i = 0;
  char rembuf[20]; /* accumulates -psmtin, etc. */
  int rembuf_i = 0;
  char *bufptr; /* we make use of indirection to simplify the code */
  int *bufptr_i;

  char addstr[BUFSIZE]; /* accumulates MODE parameters to add */
  int addstr_i;
  char remstr[BUFSIZE]; /* accumulates MODE parameters to remove */
  int remstr_i;
  char *strptr; /* more indirection to simplify the code */
  int *strptr_i;

  int totalbuflen = BUFSIZE - 200; /* fuzz factor -- don't overrun buffer! */
  int tmp;

  char limitbuf[20]; /* convert limits to strings */

  unsigned int limitdel = MODE_LIMIT;

  assert(0 != mbuf);

  /* If the ModeBuf is empty, we have nothing to do */
  if (mbuf->mb_add == 0 && mbuf->mb_rem == 0 && mbuf->mb_count == 0)
    return 0;

  /* Ok, if we were given the OPMODE flag, hide the source if its a user */
  if (mbuf->mb_dest & MODEBUF_DEST_OPMODE && !IsServer(mbuf->mb_source))
    app_source = mbuf->mb_source->user->server;
  else
    app_source = mbuf->mb_source;

  /*
   * Account for user we're bouncing; we have to get it in on the first
   * bounced MODE, or we could have problems
   */
  if (mbuf->mb_dest & MODEBUF_DEST_DEOP)
    totalbuflen -= 6; /* numeric nick == 5, plus one space */

  /* Calculate the simple flags */
  for (flag_p = flags; flag_p[0]; flag_p += 2) {
    if (*flag_p & mbuf->mb_add)
      addbuf[addbuf_i++] = flag_p[1];
    else if (*flag_p & mbuf->mb_rem)
      rembuf[rembuf_i++] = flag_p[1];
  }

  /* Now go through the modes with arguments... */
  for (i = 0; i < mbuf->mb_count; i++) {
    if (MB_TYPE(mbuf, i) & MODE_ADD) { /* adding or removing? */
      bufptr = addbuf;
      bufptr_i = &addbuf_i;
    } else {
      bufptr = rembuf;
      bufptr_i = &rembuf_i;
    }

    if (MB_TYPE(mbuf, i) & (MODE_CHANOP | MODE_VOICE)) {
      tmp = strlen(MB_CLIENT(mbuf, i)->name);

      if ((totalbuflen - IRCD_MAX(5, tmp)) <= 0) /* don't overflow buffer */
	MB_TYPE(mbuf, i) |= MODE_SAVE; /* save for later */
      else {
	bufptr[(*bufptr_i)++] = MB_TYPE(mbuf, i) & MODE_CHANOP ? 'o' : 'v';
	totalbuflen -= IRCD_MAX(5, tmp) + 1;
      }
    } else if (MB_TYPE(mbuf, i) & (MODE_KEY | MODE_BAN)) {
      tmp = strlen(MB_STRING(mbuf, i));

      if ((totalbuflen - tmp) <= 0) /* don't overflow buffer */
	MB_TYPE(mbuf, i) |= MODE_SAVE; /* save for later */
      else {
	bufptr[(*bufptr_i)++] = MB_TYPE(mbuf, i) & MODE_KEY ? 'k' : 'b';
	totalbuflen -= tmp + 1;
      }
    } else if (MB_TYPE(mbuf, i) & MODE_LIMIT) {
      /* if it's a limit, we also format the number */
      sprintf_irc(limitbuf, "%d", MB_UINT(mbuf, i));

      tmp = strlen(limitbuf);

      if ((totalbuflen - tmp) <= 0) /* don't overflow buffer */
	MB_TYPE(mbuf, i) |= MODE_SAVE; /* save for later */
      else {
	bufptr[(*bufptr_i)++] = 'l';
	totalbuflen -= tmp + 1;
      }
    }
  }

  /* terminate the mode strings */
  addbuf[addbuf_i] = '\0';
  rembuf[rembuf_i] = '\0';

  /* If we're building a user visible MODE or HACK... */
  if (mbuf->mb_dest & (MODEBUF_DEST_CHANNEL | MODEBUF_DEST_HACK2 |
		       MODEBUF_DEST_HACK3   | MODEBUF_DEST_HACK4 |
		       MODEBUF_DEST_LOG)) {
    /* Set up the parameter strings */
    addstr[0] = '\0';
    addstr_i = 0;
    remstr[0] = '\0';
    remstr_i = 0;

    for (i = 0; i < mbuf->mb_count; i++) {
      if (MB_TYPE(mbuf, i) & MODE_SAVE)
	continue;

      if (MB_TYPE(mbuf, i) & MODE_ADD) { /* adding or removing? */
	strptr = addstr;
	strptr_i = &addstr_i;
      } else {
	strptr = remstr;
	strptr_i = &remstr_i;
      }

      /* deal with clients... */
      if (MB_TYPE(mbuf, i) & (MODE_CHANOP | MODE_VOICE))
	build_string(strptr, strptr_i, MB_CLIENT(mbuf, i)->name, 0, ' ');

      /* deal with strings... */
      else if (MB_TYPE(mbuf, i) & (MODE_KEY | MODE_BAN))
	build_string(strptr, strptr_i, MB_STRING(mbuf, i), 0, ' ');

      /*
       * deal with limit; note we cannot include the limit parameter if we're
       * removing it
       */
      else if ((MB_TYPE(mbuf, i) & (MODE_ADD | MODE_LIMIT)) ==
	       (MODE_ADD | MODE_LIMIT))
	build_string(strptr, strptr_i, limitbuf, 0, ' ');
    }

    /* send the messages off to their destination */
    if (mbuf->mb_dest & MODEBUF_DEST_HACK2) {
      sendto_opmask_butone(0, SNO_HACK2, "HACK(2): %s MODE %s %s%s%s%s%s%s "
			   "[%Tu]", app_source->name, mbuf->mb_channel->chname,
			   rembuf_i ? "-" : "", rembuf, addbuf_i ? "+" : "",
			   addbuf, remstr, addstr,
			   mbuf->mb_channel->creationtime);
      sendcmdto_serv_butone(&me, CMD_DESYNCH, mbuf->mb_connect,
			    ":HACK: %s MODE %s %s%s%s%s%s%s [%Tu]",
			    app_source->name, mbuf->mb_channel->chname,
			    rembuf_i ? "-" : "", rembuf,
			    addbuf_i ? "+" : "", addbuf, remstr, addstr,
			    mbuf->mb_channel->creationtime);
    }

    if (mbuf->mb_dest & MODEBUF_DEST_HACK3)
      sendto_opmask_butone(0, SNO_HACK3, "BOUNCE or HACK(3): %s MODE %s "
			   "%s%s%s%s%s%s [%Tu]", app_source->name,
			   mbuf->mb_channel->chname, rembuf_i ? "-" : "",
			   rembuf, addbuf_i ? "+" : "", addbuf, remstr, addstr,
			   mbuf->mb_channel->creationtime);

    if (mbuf->mb_dest & MODEBUF_DEST_HACK4)
      sendto_opmask_butone(0, SNO_HACK4, "HACK(4): %s MODE %s %s%s%s%s%s%s "
			   "[%Tu]", app_source->name, mbuf->mb_channel->chname,
			   rembuf_i ? "-" : "", rembuf, addbuf_i ? "+" : "",
			   addbuf, remstr, addstr,
			   mbuf->mb_channel->creationtime);

#ifdef OPATH
    if (mbuf->mb_dest & MODEBUF_DEST_LOG) {
      write_log(OPATH, "%Tu %#C OPMODE %H %s%s%s%s%s%s\n", TStime(),
		mbuf->mb_source, mbuf->mb_channel, rembuf_i ? "-" : "", rembuf,
		addbuf_i ? "+" : "", addbuf, remstr, addstr);
    }
#endif

    if (mbuf->mb_dest & MODEBUF_DEST_CHANNEL)
      sendcmdto_channel_butserv(app_source, CMD_MODE, mbuf->mb_channel,
				"%H %s%s%s%s%s%s", mbuf->mb_channel,
				rembuf_i ? "-" : "", rembuf,
				addbuf_i ? "+" : "", addbuf, remstr, addstr);
  }

  /* Now are we supposed to propagate to other servers? */
  if (mbuf->mb_dest & MODEBUF_DEST_SERVER) {
    /* set up parameter string */
    addstr[0] = '\0';
    addstr_i = 0;
    remstr[0] = '\0';
    remstr_i = 0;

    /*
     * limit is supressed if we're removing it; we have to figure out which
     * direction is the direction for it to be removed, though...
     */
    limitdel |= (mbuf->mb_dest & MODEBUF_DEST_HACK2) ? MODE_DEL : MODE_ADD;

    for (i = 0; i < mbuf->mb_count; i++) {
      if (MB_TYPE(mbuf, i) & MODE_SAVE)
	continue;

      if (MB_TYPE(mbuf, i) & MODE_ADD) { /* adding or removing? */
	strptr = addstr;
	strptr_i = &addstr_i;
      } else {
	strptr = remstr;
	strptr_i = &remstr_i;
      }

      /* deal with modes that take clients */
      if (MB_TYPE(mbuf, i) & (MODE_CHANOP | MODE_VOICE))
	build_string(strptr, strptr_i, NumNick(MB_CLIENT(mbuf, i)), ' ');

      /* deal with modes that take strings */
      else if (MB_TYPE(mbuf, i) & (MODE_KEY | MODE_BAN))
	build_string(strptr, strptr_i, MB_STRING(mbuf, i), 0, ' ');

      /*
       * deal with the limit.  Logic here is complicated; if HACK2 is set,
       * we're bouncing the mode, so sense is reversed, and we have to
       * include the original limit if it looks like it's being removed
       */
      else if ((MB_TYPE(mbuf, i) & limitdel) == limitdel)
	build_string(strptr, strptr_i, limitbuf, 0, ' ');
    }

    /* we were told to deop the source */
    if (mbuf->mb_dest & MODEBUF_DEST_DEOP) {
      addbuf[addbuf_i++] = 'o'; /* remember, sense is reversed */
      addbuf[addbuf_i] = '\0'; /* terminate the string... */
      build_string(addstr, &addstr_i, NumNick(mbuf->mb_source), ' ');

      /* mark that we've done this, so we don't do it again */
      mbuf->mb_dest &= ~MODEBUF_DEST_DEOP;
    }

    if (mbuf->mb_dest & MODEBUF_DEST_OPMODE) {
      /* If OPMODE was set, we're propagating the mode as an OPMODE message */
      sendcmdto_serv_butone(mbuf->mb_source, CMD_OPMODE, mbuf->mb_connect,
			    "%H %s%s%s%s%s%s", mbuf->mb_channel,
			    rembuf_i ? "-" : "", rembuf, addbuf_i ? "+" : "",
			    addbuf, remstr, addstr);
    } else if (mbuf->mb_dest & MODEBUF_DEST_BOUNCE) {
      /*
       * If HACK2 was set, we're bouncing; we send the MODE back to the
       * connection we got it from with the senses reversed and a TS of 0;
       * origin is us
       */
      sendcmdto_one(&me, CMD_MODE, mbuf->mb_connect, "%H %s%s%s%s%s%s %Tu",
		    mbuf->mb_channel, addbuf_i ? "-" : "", addbuf,
		    rembuf_i ? "+" : "", rembuf, addstr, remstr,
		    mbuf->mb_channel->creationtime);
    } else {
      /*
       * We're propagating a normal MODE command to the rest of the network;
       * we send the actual channel TS unless this is a HACK3 or a HACK4
       */
      if (IsServer(mbuf->mb_source))
	sendcmdto_serv_butone(mbuf->mb_source, CMD_MODE, mbuf->mb_connect,
			      "%H %s%s%s%s%s%s %Tu", mbuf->mb_channel,
			      rembuf_i ? "-" : "", rembuf, addbuf_i ? "+" : "",
			      addbuf, remstr, addstr,
			      (mbuf->mb_dest & MODEBUF_DEST_HACK4) ? 0 :
			      mbuf->mb_channel->creationtime);
      else
	sendcmdto_serv_butone(mbuf->mb_source, CMD_MODE, mbuf->mb_connect,
			      "%H %s%s%s%s%s%s", mbuf->mb_channel,
			      rembuf_i ? "-" : "", rembuf, addbuf_i ? "+" : "",
			      addbuf, remstr, addstr);
    }
  }

  /* We've drained the ModeBuf... */
  mbuf->mb_add = 0;
  mbuf->mb_rem = 0;
  mbuf->mb_count = 0;

  /* reinitialize the mode-with-arg slots */
  for (i = 0; i < MAXMODEPARAMS; i++) {
    /* If we saved any, pack them down */
    if (MB_TYPE(mbuf, i) & MODE_SAVE) {
      mbuf->mb_modeargs[mbuf->mb_count] = mbuf->mb_modeargs[i];
      MB_TYPE(mbuf, mbuf->mb_count) &= ~MODE_SAVE; /* don't save anymore */

      if (mbuf->mb_count++ == i) /* don't overwrite our hard work */
	continue;
    } else if (MB_TYPE(mbuf, i) & MODE_FREE)
      MyFree(MB_STRING(mbuf, i)); /* free string if needed */

    MB_TYPE(mbuf, i) = 0;
    MB_UINT(mbuf, i) = 0;
  }

  /* If we're supposed to flush it all, do so--all hail tail recursion */
  if (all && mbuf->mb_count)
    return modebuf_flush_int(mbuf, 1);

  return 0;
}

/*
 * This routine just initializes a ModeBuf structure with the information
 * needed and the options given.
 */
void
modebuf_init(struct ModeBuf *mbuf, struct Client *source,
	     struct Client *connect, struct Channel *chan, unsigned int dest)
{
  int i;

  assert(0 != mbuf);
  assert(0 != source);
  assert(0 != chan);
  assert(0 != dest);

  mbuf->mb_add = 0;
  mbuf->mb_rem = 0;
  mbuf->mb_source = source;
  mbuf->mb_connect = connect;
  mbuf->mb_channel = chan;
  mbuf->mb_dest = dest;
  mbuf->mb_count = 0;

  /* clear each mode-with-parameter slot */
  for (i = 0; i < MAXMODEPARAMS; i++) {
    MB_TYPE(mbuf, i) = 0;
    MB_UINT(mbuf, i) = 0;
  }
}

/*
 * This routine simply adds modes to be added or deleted; do a binary OR
 * with either MODE_ADD or MODE_DEL
 */
void
modebuf_mode(struct ModeBuf *mbuf, unsigned int mode)
{
  assert(0 != mbuf);
  assert(0 != (mode & (MODE_ADD | MODE_DEL)));

  mode &= (MODE_ADD | MODE_DEL | MODE_PRIVATE | MODE_SECRET | MODE_MODERATED |
	   MODE_TOPICLIMIT | MODE_INVITEONLY | MODE_NOPRIVMSGS);

  if (mode & MODE_ADD) {
    mbuf->mb_rem &= ~mode;
    mbuf->mb_add |= mode;
  } else {
    mbuf->mb_add &= ~mode;
    mbuf->mb_rem |= mode;
  }
}

/*
 * This routine adds a mode to be added or deleted that takes a unsigned
 * int parameter; mode may *only* be the relevant mode flag ORed with one
 * of MODE_ADD or MODE_DEL
 */
void
modebuf_mode_uint(struct ModeBuf *mbuf, unsigned int mode, unsigned int uint)
{
  assert(0 != mbuf);
  assert(0 != (mode & (MODE_ADD | MODE_DEL)));

  MB_TYPE(mbuf, mbuf->mb_count) = mode;
  MB_UINT(mbuf, mbuf->mb_count) = uint;

  /* when we've reached the maximal count, flush the buffer */
  if (++mbuf->mb_count >=
      (MAXMODEPARAMS - (mbuf->mb_dest & MODEBUF_DEST_DEOP ? 1 : 0)))
    modebuf_flush_int(mbuf, 0);
}

/*
 * This routine adds a mode to be added or deleted that takes a string
 * parameter; mode may *only* be the relevant mode flag ORed with one of
 * MODE_ADD or MODE_DEL
 */
void
modebuf_mode_string(struct ModeBuf *mbuf, unsigned int mode, char *string,
		    int free)
{
  assert(0 != mbuf);
  assert(0 != (mode & (MODE_ADD | MODE_DEL)));

  MB_TYPE(mbuf, mbuf->mb_count) = mode | (free ? MODE_FREE : 0);
  MB_STRING(mbuf, mbuf->mb_count) = string;

  /* when we've reached the maximal count, flush the buffer */
  if (++mbuf->mb_count >=
      (MAXMODEPARAMS - (mbuf->mb_dest & MODEBUF_DEST_DEOP ? 1 : 0)))
    modebuf_flush_int(mbuf, 0);
}

/*
 * This routine adds a mode to be added or deleted that takes a client
 * parameter; mode may *only* be the relevant mode flag ORed with one of
 * MODE_ADD or MODE_DEL
 */
void
modebuf_mode_client(struct ModeBuf *mbuf, unsigned int mode,
		    struct Client *client)
{
  assert(0 != mbuf);
  assert(0 != (mode & (MODE_ADD | MODE_DEL)));

  MB_TYPE(mbuf, mbuf->mb_count) = mode;
  MB_CLIENT(mbuf, mbuf->mb_count) = client;

  /* when we've reached the maximal count, flush the buffer */
  if (++mbuf->mb_count >=
      (MAXMODEPARAMS - (mbuf->mb_dest & MODEBUF_DEST_DEOP ? 1 : 0)))
    modebuf_flush_int(mbuf, 0);
}

/*
 * This is the exported binding for modebuf_flush()
 */
int
modebuf_flush(struct ModeBuf *mbuf)
{
  return modebuf_flush_int(mbuf, 1);
}

/*
 * Simple function to invalidate bans
 */
void
mode_ban_invalidate(struct Channel *chan)
{
  struct Membership *member;

  for (member = chan->members; member; member = member->next_member)
    ClearBanValid(member);
}

/*
 * Simple function to drop invite structures
 */
void
mode_invite_clear(struct Channel *chan)
{
  while (chan->invites)
    del_invite(chan->invites->value.cptr, chan);
}

/* What we've done for mode_parse so far... */
#define DONE_LIMIT	0x01	/* We've set the limit */
#define DONE_KEY	0x02	/* We've set the key */
#define DONE_BANLIST	0x04	/* We've sent the ban list */
#define DONE_NOTOPER	0x08	/* We've sent a "Not oper" error */
#define DONE_BANCLEAN	0x10	/* We've cleaned bans... */

struct ParseState {
  struct ModeBuf *mbuf;
  struct Client *cptr;
  struct Client *sptr;
  struct Channel *chptr;
  int parc;
  char **parv;
  unsigned int flags;
  unsigned int dir;
  unsigned int done;
  int args_used;
  int max_args;
  int numbans;
  struct SLink banlist[MAXPARA];
  struct {
    unsigned int flag;
    struct Client *client;
  } cli_change[MAXPARA];
};

/*
 * Here's a helper function to deal with sending along "Not oper" or
 * "Not member" messages
 */
static void
send_notoper(struct ParseState *state)
{
  if (state->done & DONE_NOTOPER)
    return;

  send_reply(state->sptr, (state->flags & MODE_PARSE_NOTOPER) ?
	     ERR_CHANOPRIVSNEEDED : ERR_NOTONCHANNEL, state->chptr->chname);

  state->done |= DONE_NOTOPER;
}

/*
 * Helper function to convert limits
 */
static void
mode_parse_limit(struct ParseState *state, int *flag_p)
{
  unsigned int t_limit;

  if (state->dir == MODE_ADD) { /* convert arg only if adding limit */
    if (MyUser(state->sptr) && state->max_args <= 0) /* too many args? */
      return;

    if (state->parc <= 0) { /* warn if not enough args */
      if (MyUser(state->sptr))
	need_more_params(state->sptr, "MODE +l");
      return;
    }

    t_limit = atoi(state->parv[state->args_used++]); /* grab arg */
    state->parc--;
    state->max_args--;

    if (!t_limit) /* if it was zero, ignore it */
      return;
  } else
    t_limit = state->chptr->mode.limit;

  /* If they're not an oper, they can't change modes */
  if (state->flags & (MODE_PARSE_NOTOPER | MODE_PARSE_NOTMEMBER)) {
    send_notoper(state);
    return;
  }

  if (state->done & DONE_LIMIT) /* allow limit to be set only once */
    return;
  state->done |= DONE_LIMIT;

  assert(0 != state->mbuf);

  modebuf_mode_uint(state->mbuf, state->dir | flag_p[0], t_limit);

  if (state->flags & MODE_PARSE_SET) { /* set the limit */
    if (state->dir & MODE_ADD) {
      state->chptr->mode.mode |= flag_p[0];
      state->chptr->mode.limit = t_limit;
    } else {
      state->chptr->mode.mode &= flag_p[0];
      state->chptr->mode.limit = 0;
    }
  }
}

/*
 * Helper function to convert keys
 */
static void
mode_parse_key(struct ParseState *state, int *flag_p)
{
  char *t_str, *s;
  int t_len;

  if (MyUser(state->sptr) && state->max_args <= 0) /* drop if too many args */
    return;

  if (state->parc <= 0) { /* warn if not enough args */
    if (MyUser(state->sptr))
      need_more_params(state->sptr, state->dir == MODE_ADD ? "MODE +k" :
		       "MODE -k");
    return;
  }

  t_str = state->parv[state->args_used++]; /* grab arg */
  state->parc--;
  state->max_args--;

  /* If they're not an oper, they can't change modes */
  if (state->flags & (MODE_PARSE_NOTOPER | MODE_PARSE_NOTMEMBER)) {
    send_notoper(state);
    return;
  }

  if (state->done & DONE_KEY) /* allow key to be set only once */
    return;
  state->done |= DONE_KEY;

  t_len = KEYLEN + 1;

  /* clean up the key string */
  s = t_str;
  while (*++s > ' ' && *s != ':' && --t_len)
    ;
  *s = '\0';

  if (!*t_str) { /* warn if empty */
    if (MyUser(state->sptr))
      need_more_params(state->sptr, state->dir == MODE_ADD ? "MODE +k" :
		       "MODE -k");
    return;
  }

  /* can't add a key if one is set, nor can one remove the wrong key */
  if (!(state->flags & MODE_PARSE_FORCE))
    if ((state->dir == MODE_ADD && *state->chptr->mode.key) ||
	(state->dir == MODE_DEL &&
	 ircd_strcmp(state->chptr->mode.key, t_str))) {
      send_reply(state->sptr, ERR_KEYSET, state->chptr->chname);
      return;
    }

  assert(0 != state->mbuf);

  if (state->flags & MODE_PARSE_BOUNCE) {
    if (*state->chptr->mode.key) /* reset old key */
      modebuf_mode_string(state->mbuf, MODE_DEL | flag_p[0],
			  state->chptr->mode.key, 0);
    else /* remove new bogus key */
      modebuf_mode_string(state->mbuf, MODE_ADD | flag_p[0], t_str, 0);
  } else /* send new key */
    modebuf_mode_string(state->mbuf, state->dir | flag_p[0], t_str, 0);

  if (state->flags & MODE_PARSE_SET) {
    if (state->dir == MODE_ADD) /* set the new key */
      ircd_strncpy(state->chptr->mode.key, t_str, KEYLEN);
    else /* remove the old key */
      *state->chptr->mode.key = '\0';
  }
}

/*
 * Helper function to convert bans
 */
static void
mode_parse_ban(struct ParseState *state, int *flag_p)
{
  char *t_str, *s;
  struct SLink *ban, *newban = 0;

  if (state->parc <= 0) { /* Not enough args, send ban list */
    if (MyUser(state->sptr) && !(state->done & DONE_BANLIST)) {
      send_ban_list(state->sptr, state->chptr);
      state->done |= DONE_BANLIST;
    }

    return;
  }

  if (MyUser(state->sptr) && state->max_args <= 0) /* drop if too many args */
    return;

  t_str = state->parv[state->args_used++]; /* grab arg */
  state->parc--;
  state->max_args--;

  /* If they're not an oper, they can't change modes */
  if (state->flags & (MODE_PARSE_NOTOPER | MODE_PARSE_NOTMEMBER)) {
    send_notoper(state);
    return;
  }

  if ((s = strchr(t_str, ' ')))
    *s = '\0';

  if (!*t_str || *t_str == ':') { /* warn if empty */
    if (MyUser(state->sptr))
      need_more_params(state->sptr, state->dir == MODE_ADD ? "MODE +b" :
		       "MODE -b");
    return;
  }

  t_str = collapse(pretty_mask(t_str));

  /* remember the ban for the moment... */
  if (state->dir == MODE_ADD) {
    newban = state->banlist + (state->numbans++);
    newban->next = 0;

    DupString(newban->value.ban.banstr, t_str);
    newban->value.ban.who = state->sptr->name;
    newban->value.ban.when = TStime();

    newban->flags = CHFL_BAN | MODE_ADD;

    if ((s = strrchr(t_str, '@')) && check_if_ipmask(s + 1))
      newban->flags |= CHFL_BAN_IPMASK;
  }

  if (!state->chptr->banlist) {
    state->chptr->banlist = newban; /* add our ban with its flags */
    state->done |= DONE_BANCLEAN;
    return;
  }

  /* Go through all bans */
  for (ban = state->chptr->banlist; ban; ban = ban->next) {
    /* first, clean the ban flags up a bit */
    if (!(state->done & DONE_BANCLEAN))
      /* Note: We're overloading *lots* of bits here; be careful! */
      ban->flags &= ~(MODE_ADD | MODE_DEL | CHFL_BAN_OVERLAPPED);

    /* Bit meanings:
     *
     * MODE_ADD		   - Ban was added; if we're bouncing modes,
     *			     then we'll remove it below; otherwise,
     *			     we'll have to allocate a real ban
     *
     * MODE_DEL		   - Ban was marked for deletion; if we're
     *			     bouncing modes, we'll have to re-add it,
     *			     otherwise, we'll have to remove it
     *
     * CHFL_BAN_OVERLAPPED - The ban we added turns out to overlap
     *			     with a ban already set; if we're
     *			     bouncing modes, we'll have to bounce
     *			     this one; otherwise, we'll just ignore
     *			     it when we process added bans
     */

    if (state->dir == MODE_DEL && !ircd_strcmp(ban->value.ban.banstr, t_str)) {
      ban->flags |= MODE_DEL; /* delete one ban */

      if (state->done & DONE_BANCLEAN) /* If we're cleaning, finish */
	break;
    } else if (state->dir == MODE_ADD) {
      /* if the ban already exists, don't worry about it */
      if (!ircd_strcmp(ban->value.ban.banstr, t_str)) {
	if (state->done & DONE_BANCLEAN) /* If we're cleaning, finish */
	  break;
	continue;
      } else if (!mmatch(ban->value.ban.banstr, t_str)) {
	if (!(ban->flags & MODE_DEL))
	  newban->flags |= CHFL_BAN_OVERLAPPED; /* our ban overlaps */
      } else if (!mmatch(t_str, ban->value.ban.banstr))
	ban->flags |= MODE_DEL; /* mark ban for deletion: overlapping */

      if (!ban->next) {
	ban->next = newban; /* add our ban with its flags */
	break; /* get out of loop */
      }
    }
  }
  state->done |= DONE_BANCLEAN;
}

/*
 * This is the bottom half of the ban processor
 */
static void
mode_process_bans(struct ParseState *state)
{
  struct SLink *ban, *newban, *prevban, *nextban;
  int count = 0;
  int len = 0;
  int banlen;
  int changed = 0;

  for (prevban = 0, ban = state->chptr->banlist; ban; ban = nextban) {
    count++;
    banlen = strlen(ban->value.ban.banstr);
    len += banlen;
    nextban = ban->next;

    if ((ban->flags & (MODE_DEL | MODE_ADD)) == (MODE_DEL | MODE_ADD)) {
      if (prevban)
	prevban->next = 0; /* Break the list; ban isn't a real ban */
      else
	state->chptr->banlist = 0;

      count--;
      len -= banlen;

      MyFree(ban->value.ban.banstr);

      continue;
    } else if (ban->flags & MODE_DEL) { /* Deleted a ban? */
      modebuf_mode_string(state->mbuf, MODE_DEL | MODE_BAN,
			  ban->value.ban.banstr,
			  state->flags & MODE_PARSE_SET);

      if (state->flags & MODE_PARSE_SET) { /* Ok, make it take effect */
	if (prevban) /* clip it out of the list... */
	  prevban->next = ban->next;
	else
	  state->chptr->banlist = ban->next;

	count--;
	len -= banlen;

	MyFree(ban->value.ban.who);
	free_link(ban);

	changed++;
	continue; /* next ban; keep prevban like it is */
      } else
	ban->flags &= (CHFL_BAN | CHFL_BAN_IPMASK); /* unset other flags */
    } else if (ban->flags & MODE_ADD) { /* adding a ban? */
      if (prevban)
	prevban->next = 0; /* Break the list; ban isn't a real ban */
      else
	state->chptr->banlist = 0;

      /* If we're supposed to ignore it, do so. */
      if (ban->flags & CHFL_BAN_OVERLAPPED &&
	  !(state->flags & MODE_PARSE_BOUNCE)) {
	count--;
	len -= banlen;

	MyFree(ban->value.ban.banstr);
      } else {
	if (state->flags & MODE_PARSE_SET && MyUser(state->sptr) &&
	    (len > MAXBANLENGTH || count >= MAXBANS)) {
	  send_reply(state->sptr, ERR_BANLISTFULL, state->chptr->chname,
		     ban->value.ban.banstr);
	  count--;
	  len -= banlen;

	  MyFree(ban->value.ban.banstr);
	} else {
	  /* add the ban to the buffer */
	  modebuf_mode_string(state->mbuf, MODE_ADD | MODE_BAN,
			      ban->value.ban.banstr,
			      !(state->flags & MODE_PARSE_SET));

	  if (state->flags & MODE_PARSE_SET) { /* create a new ban */
	    newban = make_link();
	    newban->value.ban.banstr = ban->value.ban.banstr;
	    DupString(newban->value.ban.who, ban->value.ban.who);
	    newban->value.ban.when = ban->value.ban.when;
	    newban->flags = ban->flags & (CHFL_BAN | CHFL_BAN_IPMASK);

	    newban->next = state->chptr->banlist; /* and link it in */
	    state->chptr->banlist = newban;

	    changed++;
	  }
	}
      }
    }

    prevban = ban;
  } /* for (prevban = 0, ban = state->chptr->banlist; ban; ban = nextban) { */

  if (changed) /* if we changed the ban list, we must invalidate the bans */
    mode_ban_invalidate(state->chptr);
}

/*
 * Helper function to process client changes
 */
static void
mode_parse_client(struct ParseState *state, int *flag_p)
{
  char *t_str;
  struct Client *acptr;
  int i;

  if (MyUser(state->sptr) && state->max_args <= 0) /* drop if too many args */
    return;

  if (state->parc <= 0) { /* warn if not enough args */
    if (MyUser(state->sptr))
      need_more_params(state->sptr, state->dir == MODE_ADD ?
		       (flag_p[0] == MODE_CHANOP ? "MODE +o" : "MODE +v") :
		       (flag_p[0] == MODE_CHANOP ? "MODE -o" : "MODE -v"));
    return;
  }

  t_str = state->parv[state->args_used++]; /* grab arg */
  state->parc--;
  state->max_args--;

  /* If they're not an oper, they can't change modes */
  if (state->flags & (MODE_PARSE_NOTOPER | MODE_PARSE_NOTMEMBER)) {
    send_notoper(state);
    return;
  }

  if (MyUser(state->sptr)) /* find client we're manipulating */
    acptr = find_chasing(state->sptr, t_str, NULL);
  else
    acptr = findNUser(t_str);

  if (!acptr)
    return; /* find_chasing() already reported an error to the user */

  for (i = 0; i < MAXPARA; i++) /* find an element to stick them in */
    if (!state->cli_change[i].flag || (state->cli_change[i].client == acptr &&
				       state->cli_change[i].flag & flag_p[0]))
      break; /* found a slot */

  /* Store what we're doing to them */
  state->cli_change[i].flag = state->dir | flag_p[0];
  state->cli_change[i].client = acptr;
}

/*
 * Helper function to process the changed client list
 */
static void
mode_process_clients(struct ParseState *state)
{
  int i;
  struct Membership *member;

  for (i = 0; state->cli_change[i].flag; i++) {
    assert(0 != state->cli_change[i].client);

    /* look up member link */
    if (!(member = find_member_link(state->chptr,
				    state->cli_change[i].client)) ||
	(MyUser(state->sptr) && IsZombie(member))) {
      if (MyUser(state->sptr))
	send_reply(state->sptr, ERR_USERNOTINCHANNEL,
		   state->cli_change[i].client->name, state->chptr->chname);
      continue;
    }

    if ((state->cli_change[i].flag & MODE_ADD &&
	 (state->cli_change[i].flag & member->status)) ||
	(state->cli_change[i].flag & MODE_DEL &&
	 !(state->cli_change[i].flag & member->status)))
      continue; /* no change made, don't do anything */

    /* see if the deop is allowed */
    if ((state->cli_change[i].flag & (MODE_DEL | MODE_CHANOP)) ==
	(MODE_DEL | MODE_CHANOP)) {
      /* prevent +k users from being deopped */
      if (IsChannelService(state->cli_change[i].client)) {
	if (state->flags & MODE_PARSE_FORCE) /* it was forced */
	  sendto_opmask_butone(0, SNO_HACK4, "Deop of +k user on %H by %s",
			       state->chptr,
			       (IsServer(state->sptr) ? state->sptr->name :
				state->sptr->user->server->name));

	else if (MyUser(state->sptr) && state->flags & MODE_PARSE_SET) {
	  send_reply(state->sptr, ERR_ISCHANSERVICE,
		     state->cli_change[i].client->name, state->chptr->chname);
	  continue;
	}
      }

#ifdef NO_OPER_DEOP_LCHAN
      /* don't allow local opers to be deopped on local channels */
      if (MyUser(state->sptr) && state->cli_change[i].client != state->sptr &&
	  IsOperOnLocalChannel(state->cli_change[i].client,
			       state->chptr->chname)) {
	send_reply(state->sptr, ERR_ISOPERLCHAN,
		   state->cli_change[i].client->name, state->chptr->chname);
	continue;
      }
#endif
    }

    /* accumulate the change */
    modebuf_mode_client(state->mbuf, state->cli_change[i].flag,
			state->cli_change[i].client);

    /* actually effect the change */
    if (state->flags & MODE_PARSE_SET) {
      if (state->cli_change[i].flag & MODE_ADD) {
	member->status |= (state->cli_change[i].flag &
			   (MODE_CHANOP | MODE_VOICE));
	if (state->cli_change[i].flag & MODE_CHANOP)
	  ClearDeopped(member);
      } else
	member->status &= ~(state->cli_change[i].flag &
			    (MODE_CHANOP | MODE_VOICE));
    }
  } /* for (i = 0; state->cli_change[i].flags; i++) { */
}

/*
 * Helper function to process the simple modes
 */
static void
mode_parse_mode(struct ParseState *state, int *flag_p)
{
  if ((state->dir == MODE_ADD &&  (flag_p[0] & state->chptr->mode.mode)) ||
      (state->dir == MODE_DEL && !(flag_p[0] & state->chptr->mode.mode)))
    return; /* no change */

  /* If they're not an oper, they can't change modes */
  if (state->flags & (MODE_PARSE_NOTOPER | MODE_PARSE_NOTMEMBER)) {
    send_notoper(state);
    return;
  }

  assert(0 != state->mbuf);

  modebuf_mode(state->mbuf, state->dir | flag_p[0]);

  /* make +p and +s mutually exclusive */
  if (state->dir == MODE_ADD && flag_p[0] & (MODE_SECRET | MODE_PRIVATE)) {
    if (flag_p[0] == MODE_SECRET && (state->chptr->mode.mode & MODE_PRIVATE))
      modebuf_mode(state->mbuf, MODE_DEL | MODE_PRIVATE);
    else if (flag_p[0] == MODE_PRIVATE &&
	     (state->chptr->mode.mode & MODE_SECRET))
      modebuf_mode(state->mbuf, MODE_DEL | MODE_SECRET);
  }

  if (state->flags & MODE_PARSE_SET) { /* set the flags */
    if (state->dir == MODE_ADD) { /* add the mode to the channel */
      state->chptr->mode.mode |= flag_p[0];

      /* make +p and +s mutually exclusive */
      if (state->dir == MODE_ADD && flag_p[0] & (MODE_SECRET | MODE_PRIVATE)) {
	if (flag_p[0] == MODE_PRIVATE)
	  state->chptr->mode.mode &= ~MODE_SECRET;
	else
	  state->chptr->mode.mode &= ~MODE_PRIVATE;
      }
    } else /* remove the mode from the channel */
      state->chptr->mode.mode &= ~flag_p[0];
  }

  /* Clear out invite structures if we're removing invites */
  if (state->flags & MODE_PARSE_SET && state->dir == MODE_DEL &&
      flag_p[0] == MODE_INVITEONLY)
    mode_invite_clear(state->chptr);
}

/*
 * This routine is intended to parse MODE or OPMODE commands and effect the
 * changes (or just build the bounce buffer).  We pass the starting offset
 * as a 
 */
int
mode_parse(struct ModeBuf *mbuf, struct Client *cptr, struct Client *sptr,
	   struct Channel *chptr, int parc, char *parv[], unsigned int flags)
{
  static int chan_flags[] = {
    MODE_CHANOP,	'o',
    MODE_VOICE,		'v',
    MODE_PRIVATE,	'p',
    MODE_SECRET,	's',
    MODE_MODERATED,	'm',
    MODE_TOPICLIMIT,	't',
    MODE_INVITEONLY,	'i',
    MODE_NOPRIVMSGS,	'n',
    MODE_KEY,		'k',
    MODE_BAN,		'b',
    MODE_LIMIT,		'l',
    MODE_ADD,		'+',
    MODE_DEL,		'-',
    0x0, 0x0
  };
  int i;
  int *flag_p;
  char *modestr;
  struct ParseState state;

  assert(0 != cptr);
  assert(0 != sptr);
  assert(0 != chptr);
  assert(0 != parc);
  assert(0 != parv);

  state.mbuf = mbuf;
  state.cptr = cptr;
  state.sptr = sptr;
  state.chptr = chptr;
  state.parc = parc;
  state.parv = parv;
  state.flags = flags;
  state.dir = MODE_ADD;
  state.done = 0;
  state.args_used = 0;
  state.max_args = MAXMODEPARAMS;
  state.numbans = 0;

  for (i = 0; i < MAXPARA; i++) { /* initialize ops/voices arrays */
    state.banlist[i].next = 0;
    state.banlist[i].value.ban.banstr = 0;
    state.banlist[i].value.ban.who = 0;
    state.banlist[i].value.ban.when = 0;
    state.banlist[i].flags = 0;
    state.cli_change[i].flag = 0;
    state.cli_change[i].client = 0;
  }

  modestr = state.parv[state.args_used++];
  state.parc--;

  while (*modestr) {
    for (; *modestr; modestr++) {
      for (flag_p = chan_flags; flag_p[0]; flag_p += 2) /* look up flag */
	if (flag_p[1] == *modestr)
	  break;

      if (!flag_p[0]) { /* didn't find it?  complain and continue */
	if (MyUser(state.sptr))
	  send_reply(state.sptr, ERR_UNKNOWNMODE, *modestr);
	continue;
      }

      switch (*modestr) {
      case '+': /* switch direction to MODE_ADD */
      case '-': /* switch direction to MODE_DEL */
	state.dir = flag_p[0];
	break;

      case 'l': /* deal with limits */
	mode_parse_limit(&state, flag_p);
	break;

      case 'k': /* deal with keys */
	mode_parse_key(&state, flag_p);
	break;

      case 'b': /* deal with bans */
	mode_parse_ban(&state, flag_p);
	break;

      case 'o': /* deal with ops/voice */
      case 'v':
	mode_parse_client(&state, flag_p);
	break;

      default: /* deal with other modes */
	mode_parse_mode(&state, flag_p);
	break;
      } /* switch (*modestr) { */
    } /* for (; *modestr; modestr++) { */

    if (state.parc > 0) { /* process next argument in string */
      modestr = state.parv[state.args_used++];
      state.parc--;

      /* is it a TS? */
      if (IsServer(state.sptr) && !state.parc && IsDigit(*modestr)) {
	time_t recv_ts;

	if (!(state.flags & MODE_PARSE_SET))	  /* don't set earlier TS if */
	  break;		     /* we're then going to bounce the mode! */

	recv_ts = atoi(modestr);

	if (recv_ts && recv_ts < state.chptr->creationtime)
	  state.chptr->creationtime = recv_ts; /* respect earlier TS */

	break; /* break out of while loop */
      } else if (state.flags & MODE_PARSE_STRICT ||
		 (MyUser(state.sptr) && state.max_args <= 0)) {
	state.parc++; /* we didn't actually gobble the argument */
	state.args_used--;
	break; /* break out of while loop */
      }
    }
  } /* while (*modestr) { */

  /*
   * the rest of the function finishes building resultant MODEs; if the
   * origin isn't a member or an oper, skip it.
   */
  if (state.flags & (MODE_PARSE_NOTOPER | MODE_PARSE_NOTMEMBER))
    return state.args_used; /* tell our parent how many args we gobbled */

  assert(0 != state.mbuf);

  if (state.done & DONE_BANCLEAN) /* process bans */
    mode_process_bans(&state);

  /* process client changes */
  if (state.cli_change[0].flag)
    mode_process_clients(&state);

  return state.args_used; /* tell our parent how many args we gobbled */
}

/*
 * Initialize a join buffer
 */
void
joinbuf_init(struct JoinBuf *jbuf, struct Client *source,
	     struct Client *connect, unsigned int type, char *comment,
	     time_t create)
{
  int i;

  assert(0 != jbuf);
  assert(0 != source);
  assert(0 != connect);

  jbuf->jb_source = source; /* just initialize struct JoinBuf */
  jbuf->jb_connect = connect;
  jbuf->jb_type = type;
  jbuf->jb_comment = comment;
  jbuf->jb_create = create;
  jbuf->jb_count = 0;
  jbuf->jb_strlen = (((type == JOINBUF_TYPE_JOIN ||
		       type == JOINBUF_TYPE_PART ||
		       type == JOINBUF_TYPE_PARTALL) ?
		      STARTJOINLEN : STARTCREATELEN) +
		     (comment ? strlen(comment) + 2 : 0));

  for (i = 0; i < MAXJOINARGS; i++)
    jbuf->jb_channels[i] = 0;
}

/*
 * Add a channel to the join buffer
 */
void
joinbuf_join(struct JoinBuf *jbuf, struct Channel *chan, unsigned int flags)
{
  unsigned int len;

  assert(0 != jbuf);

  if (chan) {
    if (jbuf->jb_type == JOINBUF_TYPE_PART ||
	jbuf->jb_type == JOINBUF_TYPE_PARTALL) {
      /* Send notification to channel */
      if (!(flags & CHFL_ZOMBIE))
	sendcmdto_channel_butserv(jbuf->jb_source, CMD_PART, chan,
				  (flags & CHFL_BANNED || !jbuf->jb_comment) ?
				  ":%H" : "%H :%s", chan, jbuf->jb_comment);
      else if (MyUser(jbuf->jb_source))
	sendcmdto_one(jbuf->jb_source, CMD_PART, jbuf->jb_source,
		      (flags & CHFL_BANNED || !jbuf->jb_comment) ?
		      ":%H" : "%H :%s", chan, jbuf->jb_comment);

      /* Remove user from channel */
      remove_user_from_channel(jbuf->jb_source, chan);
    } else {
      /* Add user to channel */
      add_user_to_channel(chan, jbuf->jb_source, flags);

      /* Send the notification to the channel */
      sendcmdto_channel_butserv(jbuf->jb_source, CMD_JOIN, chan, ":%H", chan);

      /* send an op, too, if needed */
      if (jbuf->jb_type == JOINBUF_TYPE_CREATE &&
	  !IsModelessChannel(chan->chname))
	sendcmdto_channel_butserv(jbuf->jb_source, CMD_MODE, chan, "%H +o %C",
				  chan, jbuf->jb_source);
    }

    if (jbuf->jb_type == JOINBUF_TYPE_PARTALL || IsLocalChannel(chan->chname))
      return; /* don't send to remote */
  }

  /* figure out if channel name will cause buffer to be overflowed */
  len = chan ? strlen(chan->chname) + 1 : 2;
  if (jbuf->jb_strlen + len > IRC_BUFSIZE)
    joinbuf_flush(jbuf);

  /* add channel to list of channels to send and update counts */
  jbuf->jb_channels[jbuf->jb_count++] = chan;
  jbuf->jb_strlen += len;

  /* if we've used up all slots, flush */
  if (jbuf->jb_count >= MAXJOINARGS)
    joinbuf_flush(jbuf);
}

/*
 * Flush the channel list to remote servers
 */
int
joinbuf_flush(struct JoinBuf *jbuf)
{
  char chanlist[IRC_BUFSIZE];
  int chanlist_i = 0;
  int i;

  if (!jbuf->jb_count || jbuf->jb_type == JOINBUF_TYPE_PARTALL)
    return 0; /* no joins to process */

  for (i = 0; i < jbuf->jb_count; i++) { /* build channel list */
    build_string(chanlist, &chanlist_i,
		 jbuf->jb_channels[i] ? jbuf->jb_channels[i]->chname : "0", 0,
		 i == 0 ? '\0' : ',');

    jbuf->jb_channels[i] = 0; /* mark slot empty */
  }

  jbuf->jb_count = 0; /* reset base counters */
  jbuf->jb_strlen = ((jbuf->jb_type == JOINBUF_TYPE_JOIN ||
		      jbuf->jb_type == JOINBUF_TYPE_PART ?
		      STARTJOINLEN : STARTCREATELEN) +
		     (jbuf->jb_comment ? strlen(jbuf->jb_comment) + 2 : 0));

  /* and send the appropriate command */
  switch (jbuf->jb_type) {
  case JOINBUF_TYPE_JOIN:
    sendcmdto_serv_butone(jbuf->jb_source, CMD_JOIN, jbuf->jb_connect,
			  "%s", chanlist);
    break;

  case JOINBUF_TYPE_CREATE:
    sendcmdto_serv_butone(jbuf->jb_source, CMD_CREATE, jbuf->jb_connect,
			  "%s %Tu", chanlist, jbuf->jb_create);
    break;

  case JOINBUF_TYPE_PART:
    sendcmdto_serv_butone(jbuf->jb_source, CMD_PART, jbuf->jb_connect,
			  jbuf->jb_comment ? "%s :%s" : "%s", chanlist,
			  jbuf->jb_comment);
    break;
  }

  return 0;
}
