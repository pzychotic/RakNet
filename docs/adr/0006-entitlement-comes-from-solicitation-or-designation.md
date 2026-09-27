# Entitlement comes from solicitation or designation

Status: accepted. Answers the question ADR-0005 §5 leaves open.

ADR-0005 bounds how much a System can make a Peer hold, and says that whether a System is
*entitled* to send a message is a separate question. Router2 and the UDPProxy plugins
answered it with "anyone connected": any System could redirect another connection's
address, make a Peer ping addresses of its choosing, or steer which proxy two other clients
get. A RakNetGUID cannot fix this. It is not a secret, and the first System to connect with
a given RakNetGUID holds it, so "the message names X" or "the sender claims to be X" proves
nothing.

**Decision.**

1. **A plugin acts on a remote claim only if the Message is Solicited or comes from a
   Designated System.** Where the Peer has state for the exchange (its own outstanding
   request), the Message must match it. Where it has none, as for a Router2 endpoint or a
   UDPProxy target that never asked for anything, only a Designated System may send it.
2. **Nothing is designated by default, and undesignated means refused.** A plugin whose
   application has designated no one ignores every unsolicited claim. That breaks embedders
   who relied on the old behaviour, which is deliberate: accepting anyone by default is the
   defect.
3. **Designation is by connected address, per role.** Each plugin exposes a set of
   `SystemAddress`es for each role it trusts (`AddX`/`RemoveX`). A designation lapses when
   the connection at that address closes, so a later System that reuses the address inherits
   nothing. There is no predicate callback, because `return true;` would undo this decision.
4. **A claim about a third party is not honoured.** A requester can only speak for itself.
   A UDPProxy forwarding request's source is always the requester's own address, whatever
   the Message says.

**Consequences.** A UDPProxy target behind NAT is unreachable until its application
designates the coordinator. A Router2 endpoint re-routes a live connection only for a
Designated intermediary, and never moves a direct connection. New forwarded connections are
unaffected. Both go in `MIGRATION.md`, since each breaks a working 4.081 integration
without breaking its build.

## Considered and rejected

- **Solicited only.** Needs no API, but has no answer for the party that asked for nothing,
  so either those roles break or they stay open.
- **Designated only.** Makes an application designate even where the Peer's own request
  already proves the exchange is legitimate.
- **Accept anyone when nothing is designated.** Keeps existing embedders working, and leaves
  every one of them exposed to exactly the attacks this decision closes.
- **Designate by RakNetGUID.** Whoever connects first with a RakNetGUID holds it, so the
  designation goes to whoever gets there first.
