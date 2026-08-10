# Prior appellate event catalog

This is the complete 53-event inventory extracted on 2026-08-11 from the local predecessor
repository at commit `18ac465` (`src/domain/packs.ts`). It preserves scope; it does not endorse
the predecessor's legal text, deadlines, or implementation.

Profile codes are `C` civil appeal, `K` criminal appeal, `A` agency review, and `W` original
writ. A profile code means the predecessor exposed the event in that pack, including shared
events whose source-domain field said civil appeal.

Treatment has a precise release meaning:

- **Port — required:** at least one of the nine shipped cases must provide a validated filing
  route or typed court action and an executable trace for the event.
- **Port — adverse/optional:** the declarative catalog and built-in opcodes must represent the
  event, and a case may exercise it as a sourced adverse or optional branch.
- **Defer — outside nine-case MVP:** deliberately absent from the shipped case routes. It is not
  silently counted as implemented and may be added later through data packs.
- **Supersede — accepted/sealed state:** the predecessor's standalone acknowledgment is replaced
  by a filing-acceptance event plus the typed sealed-state transition; no duplicate pseudo-filing
  will be carried forward.

| Prior event ID | Prior label | Prior profiles | MVP treatment |
| --- | --- | --- | --- |
| `notice_of_appeal` | Notice of Appeal | C | Port — required |
| `criminal_notice_of_appeal` | Criminal Notice of Appeal | K | Port — required |
| `petition_for_review` | Petition for Review | A | Port — required |
| `petition_for_writ_mandamus` | Petition for Writ of Mandamus or Prohibition | W | Port — required |
| `appearance_disclosure` | Appearance of Counsel | C, K, A, W | Port — required |
| `disclosure_statement` | Disclosure Statement | C, K, A, W | Port — required |
| `docketing_statement` | Docketing Statement | C | Port — required |
| `criminal_docketing_statement` | Criminal Docketing Statement | K | Port — required |
| `agency_docketing_statement` | Agency-Review Docketing Statement | A | Port — required |
| `docketing_statement_objection` | Docketing Statement Objection or Correction | C, K, A | Port — adverse/optional |
| `cja_financial_disclosure` | CJA 23 Financial Affidavit | K | Port — required |
| `ifp_application` | In Forma Pauperis Affidavit | C, K, A, W | Port — adverse/optional |
| `plra_application` | PLRA Application and Fee-Collection Consent | C, K, W | Port — adverse/optional |
| `transcript_order_acknowledgment` | Transcript Order or No-Transcript Certificate | C, K | Port — required |
| `transcript_extension_request` | Transcript Extension Request | C, K | Port — adverse/optional |
| `certified_agency_record` | Agency Record or Certified List | A | Port — required |
| `motion` | Motion | C, K, A, W | Port — required |
| `motion_response` | Response to Motion | C, K, A, W | Port — required |
| `motion_reply` | Reply in Support of Motion | C, K, A | Port — adverse/optional |
| `motion_stay_pending_appeal` | Motion to Stay or for Injunction Pending Appeal | C | Port — required |
| `motion_to_supplement_record` | Motion to Supplement Record | A | Port — required |
| `emergency_motion_stay` | Emergency Motion for Stay | W | Port — required |
| `order_inviting_answer` | Order Inviting Answer | W | Port — required |
| `answer_to_writ_petition` | Answer to Writ Petition | W | Port — required |
| `reply_in_support_of_writ` | Reply in Support of Writ Petition | W | Port — required |
| `writ_disposition` | Writ Disposition | W | Port — required |
| `opening_brief` | Opening Brief | C, K, A | Port — required |
| `joint_appendix` | Joint Appendix | C, K, A | Port — required |
| `appellee_brief` | Appellee Brief | C, K, A | Port — required |
| `reply_brief` | Reply Brief | C, K, A | Port — required |
| `amicus_notice_or_consent` | Amicus Notice / Consent Statement | C | Defer — outside nine-case MVP |
| `motion_for_leave_to_file_amicus` | Motion for Leave to File Amicus Brief | C | Defer — outside nine-case MVP |
| `response_to_amicus_motion` | Response to Amicus Motion | C | Defer — outside nine-case MVP |
| `amicus_brief` | Amicus Brief | C | Defer — outside nine-case MVP |
| `corrected_brief` | Corrected Brief | C, K, A | Port — required |
| `rule_28j_letter` | Rule 28(j) Letter | C, K, A | Port — adverse/optional |
| `motion_extend_time` | Motion to Extend Time | C, K, A | Port — required |
| `motion_overlength_brief` | Motion to File Overlength Brief | C, K, A | Port — adverse/optional |
| `motion_to_seal` | Motion to Seal | C, K, A, W | Port — required |
| `sealed_filing_acknowledgment` | Sealed Filing Acknowledgment | C, K, A, W | Supersede — accepted/sealed state |
| `certificate_of_confidentiality` | Certificate of Confidentiality | C, K, A, W | Port — required |
| `highly_sensitive_document_certificate` | Highly Sensitive Document Certificate | C, K, A, W | Port — required |
| `sealed_brief` | Sealed Brief | C, K, A, W | Port — required |
| `sealed_appendix` | Sealed Appendix | C, K, A, W | Port — required |
| `oral_argument_acknowledgment` | Oral Argument Acknowledgment | C, K, A | Port — required |
| `oral_argument_conflict_notice` | Oral Argument Conflict Notice | C, K, A | Port — required |
| `mandate_stay_motion` | Motion to Stay Mandate | C, K, A | Port — adverse/optional |
| `petition_rehearing` | Panel or En Banc Rehearing Petition | C, K, A | Port — required |
| `bill_of_costs` | Bill of Costs | C, K, A | Defer — outside nine-case MVP |
| `motion_reconsider_cost_allocation` | Motion to Reconsider Cost Allocation | C, K, A | Defer — outside nine-case MVP |
| `costs_objection` | Objection to Bill of Costs | C, K, A | Defer — outside nine-case MVP |
| `certiorari_information_sheet` | Certiorari Information Sheet | C, K, A | Defer — outside nine-case MVP |
| `certiorari_status_form` | Certiorari Status Form | C, K, A | Defer — outside nine-case MVP |

The classification totals are 35 required ports, 8 adverse/optional ports, 9 deliberate
deferrals, and 1 superseded pseudo-event. Together they account for all 53 unique predecessor
definitions. The old per-profile membership counts remain 40 civil, 36 criminal, 34 agency,
and 18 writ; shared membership is why those counts sum to more than 53.
