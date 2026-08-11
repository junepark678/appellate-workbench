SYNTHETIC TRAINING RECORD — NOT FILED — ALL FACTS AND IDENTIFIERS ARE FICTIONAL

# Exhibit receipt, file audit, and correction certification
## March 3, 2025 Rule 16(b) stipulation and review

The parties signed the following complete UTF-8 stipulation bytes, with no terminal newline:

`STIPULATION-16B|date=2025-03-03|docket=SYN-CA4-25-AG-4301|parties=ARM,DHS|agree=correct omission by restoring unchanged Agency Exhibit P-7 at agency record pages 33-50;file itemized corrected index;preserve initial transmission;exclude appellate proffer pages 1-8 from administrative record|authority=Fed. R. App. P. 16(b)|signatures=ARM-C-9,DHS-APP-4`

SHA-256: `b26f4d1618332a6e006839a09c4ab77319bde4f58c6d3e219da4fbcfbe1ce855`.
Under Federal Rule of Appellate Procedure 16(b), the stipulation authorizes correction of an
omission in the review record. It is a March 3 appellate correction-process document, not evidence
presented to the immigration judge on the merits. The records office therefore compares receipts,
transcripts, decisions, and exact stored bytes without adding a new merits fact.

<!-- PAGE BREAK -->
# April 12 lodging receipt — complete table

| Exhibit | Physical file or packet pages | Pages | April status |
| --- | --- | ---: | --- |
| P-1 | 03-cat-application.pdf | 12 | lodged; merits ruling required |
| P-2 | 05-sibling-declaration.pdf | 10 | lodged; merits ruling required |
| P-3 | 06-family-corroboration.pdf | 12 | lodged; merits ruling required |
| P-4 | 09 packet pages 2–3; KAL-MSG-1 through KAL-MSG-4 | 2 | lodged; merits ruling required |
| P-5 | 09 packet pages 4–5; KAL-OCI-1 and KAL-ROUTE-1 | 2 | lodged; merits ruling required |
| P-6 | 09 packet pages 1 and 6–10; clinic, ledgers, translation, certification | 6 | lodged; merits ruling required |
| P-7 | 04-arm-sworn-declaration.pdf | 18 | lodged; merits ruling required |

The clerk receipt identifies the combined ten-page physical P-4/P-5/P-6 packet once while assigning
its ten primary pages exactly: two to P-4, two to P-5, and six to P-6. It records no merits ruling.
P-7's stored control is `04-arm-sworn-declaration.pdf`, lodged before the hearing.

<!-- PAGE BREAK -->
# September hearing receipt — complete table

| Exhibit | Exact agency pages | Status and ruling date |
| --- | --- | --- |
| P-1 | agency pages 21–32 | admitted September 17 |
| P-2 | agency pages 51–60 | admitted September 17 |
| P-3 | agency pages 61–72 | admitted September 17 |
| P-4 | agency pages 106–107 | admitted September 17 |
| P-5 | agency pages 108–109 | admitted September 17 |
| P-6 | agency page 105 and pages 110–114 | admitted September 17 |
| P-7 | agency pages 33–50 | admitted September 17; no authenticity or timeliness objection |
| P-8 | agency pages 73–92 | admitted September 17; testimony received September 18 |
| P-9 | agency pages 93–104 | admitted September 17; weight addressed September 18 |

The receipt marks every exhibit P-1 through P-9 admitted. The P-4/P-5/P-6 logical sets resolve to
the same combined physical file without overlap in their primary-page assignments. The receipt
does not make any exhibit statement automatically true; it records the court's evidentiary status.

<!-- PAGE BREAK -->
# Transcript cross-check

At agency page 117 the first hearing volume identifies P-7 as the eighteen-page declaration lodged
April 12.
DHS states no authenticity objection, no timeliness objection, and no objection to admission; the
judge admits P-7. At agency page 138 the clerk confirms admission of P-1 through P-9, specifically including
P-4 messages and P-5 complaint-routing materials, and states that no objection remains to P-7.

At agency page 139 the second volume opens by confirming that the exhibit receipt records P-7
admitted. At agency page 161 the court again confirms P-1 through P-9 admitted, including P-4 and
P-5, and states that no
part of P-7 is excluded. Those contemporaneous hearing propositions match the September table and
the stored logical mapping. The transcript is used here to verify the historic rulings, not to add
evidence outside the closed merits record.

<!-- PAGE BREAK -->
# Decision and initial-transmission cross-check

At agency page 164 the October 22 decision lists P-1 through P-9 as admitted, identifies P-7 as
eighteen pages,
states that it was lodged April 12 and admitted without authenticity or timeliness objection, and
excludes no part of it. The January 13 Board order likewise acknowledges P-7 as agency evidence.

The February packet at agency pages 219–226 is preserved unchanged. Its index jumps from the
application at pages 21–32 to S.M.'s declaration at pages 51–60. Agency page 224 records
`LABEL_DISCONTINUITY_18`, no deletion, replacement, or digest failure among selected files, and an
unresolved cause. Agency page 225 certifies the
208-page selected production as complete under an export-selection method. These comparisons prove
a conflict between the initial production and the preexisting receipts and decisions. They do not
yet assign a technical cause; the next page records the audit finding.

<!-- PAGE BREAK -->
# File-audit finding: stale status and export query

The March audit finds two P-7 status values. The stored exhibit object retained April value
`LODGED_MERITS_RULING_REQUIRED`; the September receipt table held `ADMITTED_2024-09-17`. The February
export query selected exhibit objects whose stored-object status equaled `ADMITTED`, without joining
the hearing receipt. It therefore returned no P-7 object and advanced to the next selected path.

The query log, status history, and selected-object manifest account for the eighteen-label
discontinuity. They also confirm that no P-7 deletion, byte replacement, or digest failure occurred.
The initial packet did not know or state this cause; it appears first in this March audit. The
records officer preserves both status values, records the corrected current status, and requires a
second clerk to compare all nine exhibit receipt rows against the corrected index before service.

<!-- PAGE BREAK -->
# Exact P-7 object identity

The restored object is `04-arm-sworn-declaration.pdf`, eighteen pages, titled “A.R.M. Sworn
Declaration.” Its exact SHA-256 is
`08e8294532c23fe9feb5962ca5b7780ae958178e6c8e2b4840d1ee28f3c5d212`.
That digest matches the April receipt object, the copy offered at the September hearing, and the
stored agency exhibit object. The audit opens all eighteen pages and compares the first and last
text lines of each page.

No annotation, paragraph, event, translation, or page is inserted, removed, or replaced. The
correction changes only whether the unchanged admitted object is present in the judicial-review
production. The exact digest, page count, title, and source control distinguish this operation from
supplementation with a different document.

<!-- PAGE BREAK -->
# Corrected placement and continuity

The unchanged P-7 object occupies agency pages 33–50, after the CAT application at pages 21–32 and
before S.M.'s declaration at pages 51–60. Later agency labels remain unchanged. The corrected
administrative set contains one page for every agency number 1 through 238, with no gap, duplicate
label, or page
assigned to two physical PDFs.

The combined P-4/P-5/P-6 physical file remains agency pages 105–114: P-4 is pages 106–107; P-5 is
pages 108–109; P-6 is page 105 and pages 110–114. The second clerk compares that mapping with both
receipt tables and
confirms every P-1-through-P-9 row admitted. Restoring P-7 does not change the combined packet,
transcript, decision, Board filing, or any citation already assigned after agency page 50.

<!-- PAGE BREAK -->
# Materials excluded from the Rule 16(b) correction

The audit identifies a separate T.R. declaration signed February 20, 2025 and an appellate proffer
filed February 24. Those materials postdate the January 13 Board order. They were not lodged with
the immigration court, admitted at the hearing, or presented to the Board. They remain appellate
proffer pages 1–8 on the appellate docket and receive no agency-record label.

The Rule 16(b) stipulation expressly excludes appellate proffer pages 1–8 from the administrative
record. The corrected 238-page set contains no page, text object, or digest from that proffer. This distinction preserves
correction of the unchanged admitted P-7 while leaving any request concerning new material to a
separate appellate determination. The records office makes no finding about the truth or legal
effect of the proffer.

<!-- PAGE BREAK -->
# Itemized corrected index — documents 1 through 9

| No. | File | Pages | Corrected labels |
| ---: | --- | ---: | --- |
| 1 | 01-notice-to-appear.pdf | 8 | agency 1–8 |
| 2 | 02-master-calendar-pleadings.pdf | 12 | agency 9–20 |
| 3 | 03-cat-application.pdf | 12 | agency 21–32 |
| 4 | 04-arm-sworn-declaration.pdf, P-7 | 18 | agency 33–50 |
| 5 | 05-sibling-declaration.pdf | 10 | agency 51–60 |
| 6 | 06-family-corroboration.pdf | 12 | agency 61–72 |
| 7 | 07-country-conditions-report.pdf, P-8 | 20 | agency 73–92 |
| 8 | 08-trauma-medical-evaluation.pdf, P-9 | 12 | agency 93–104 |
| 9 | 09-certified-translation-packet.pdf, combined P-4/P-5/P-6 | 10 | agency 105–114 |

Rows 1 through 9 total 114 pages. Row 9's logical page/object map is fixed by both receipt tables
and the packet certification. Row 4 carries the exact P-7 SHA-256 stated at agency page 233.

<!-- PAGE BREAK -->
# Itemized corrected index — documents 10 through 18

| No. | File | Pages | Corrected labels |
| ---: | --- | ---: | --- |
| 10 | 10-merits-hearing-transcript-vol-1.pdf | 24 | agency 115–138 |
| 11 | 11-merits-hearing-transcript-vol-2.pdf | 24 | agency 139–162 |
| 12 | 12-ij-decision.pdf | 16 | agency 163–178 |
| 13 | 13-bia-notice-appeal.pdf | 4 | agency 179–182 |
| 14 | 14-bia-opening-brief.pdf | 16 | agency 183–198 |
| 15 | 15-dhs-bia-response.pdf | 12 | agency 199–210 |
| 16 | 16-bia-final-order.pdf | 8 | agency 211–218 |
| 17 | 17-initial-certified-index.pdf | 8 | agency 219–226 |
| 18 | 18-exhibit-receipt-audit-corrected-certification.pdf | 12 | agency 227–238 |

Rows 10 through 18 total 124 pages; together both tables contain exactly eighteen unique PDFs and
238 pages. The records officer certifies this itemized index as the corrected Rule 16(b) production.
Appellate proffer pages 1–8 are excluded and do not form a nineteenth administrative-record PDF.

<!-- PAGE BREAK -->
# Corrected transmission, signatures, and retention

On March 3, 2025 the agency records officer transmits the itemized corrected index, the unchanged
P-7 object, this audit, and certification under the parties' Rule 16(b) stipulation. Both parties
receive a service notice identifying the restored agency pages 33–50, exact P-7 digest, unchanged later
labels, eighteen-file count, and 238-page count.

Records officer code ARO-16 and second-clerk code CLK-2 sign after comparing both receipt tables,
the transcript propositions, the decision, the object digest, and every index row. The initial
February selection, query, manifest, and certification remain retained as a distinct filing. The
corrected transmission contains P-7 because it was already admitted; it contains no bytes from
appellate proffer pages 1–8 and makes no merits determination concerning that separate proffer.
