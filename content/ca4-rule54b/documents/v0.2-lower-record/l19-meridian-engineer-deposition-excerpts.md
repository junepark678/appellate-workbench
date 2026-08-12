SYNTHETIC TRAINING RECORD — NOT FILED — ALL FACTS AND IDENTIFIERS ARE FICTIONAL

UNITED STATES DISTRICT COURT FOR THE NORTHERN DISTRICT OF WEST VIRGINIA

Proceeding: Meridian engineering deposition.

Asterglen Freight Software, Inc., Plaintiff,
v.
Copper Kestrel Logistics, LLC and Meridian Silt Holdings, LLC, Defendants.

Civil Action No. SYN-NDWV-25-CV-0618

# Meridian Engineer Deposition Excerpts

Deposition taken September 26, 2025. Filed November 24, 2025 as District ECF No. 49-3, an attachment to Asterglen's opposition at District ECF No. 49.

The witness was Meridian Silt's integration engineer for the Copper Kestrel engagement. The examination addressed the June 14 statement of work, identifier provisioning, participation in the July 8 call, technical events after the July 18 audit, work through July 25, and the limits of Meridian's contractual authority. The witness reviewed time entries, task tickets, configuration tables, meeting notes, and messages before testifying.

COUNSEL FOR ASTERGLEN: Please state whether an answer comes from your own participation or Meridian's files.

THE WITNESS: I personally performed or supervised much of the integration work and attended the July 8 call. I will identify matters learned from business materials.

COUNSEL FOR COPPER KESTREL: Copper Kestrel preserves objections to any suggestion that Meridian could amend the Pilot License on another party's behalf.

The witness affirmed the oath and understood that technical terms should be explained in ordinary language. The parties agreed that the excerpts did not make any participant's disputed recollection conclusive.

<!-- PAGE BREAK -->

## Examination — engagement scope

Q. Who retained Meridian?

A. Copper Kestrel retained Meridian under a statement of work dated June 14, 2024.

Q. What services were described?

A. Integration planning, terminal configuration support, validation scripts, load observation, readiness checks, and transition assistance for the proposed expanded deployment.

Q. Did Meridian have the Pilot License?

A. We received the technical schedules and were told the executed license covered twelve terminals. We also received the unsigned June 3 proposal for twenty-nine more.

Q. Did Meridian believe the proposal was signed?

A. No. Our task notes described commercial approval as pending.

Q. Did the statement of work give Meridian authority to amend Asterglen's contract?

A. No. It governed Meridian's relationship with Copper Kestrel. We could perform assigned engineering work but could not grant licenses or waive Asterglen's terms.

Q. Were you aware of the June 10 written load-test conditions?

A. Yes. Copper Kestrel instructed us to observe those conditions, monitor activity, and document results.

Q. Did you distinguish test work from full production?

A. We tagged tasks by purpose, but some event streams used realistic dispatch data. A technical log by itself does not always answer the contractual characterization.

<!-- PAGE BREAK -->

## Examination — identifier work through July 1

Q. How many identifiers were in the configuration by July 1?

A. Forty-one: twelve tied to the original schedule and twenty-nine associated with the proposed additions.

Q. Did Meridian create all of them?

A. Meridian prepared configuration inputs and validated many entries. Copper Kestrel personnel submitted or approved the administrative requests. Asterglen's platform processed the provisioning.

Q. Were all forty-one continuously active in the same manner?

A. No. Activity varied. Some identifiers carried load-test events, some were checked for routing and permissions, and some reflected ordinary dispatch interactions. The system's “active” status meant an identifier could authenticate; it did not describe every transaction's business purpose.

Q. Did Meridian add any location beyond the twenty-nine proposed additions?

A. No.

Q. Did an Asterglen signatory approve Meridian's work plan?

A. No Asterglen signatory executed the June 3 change order or the Meridian statement of work.

Q. Why did work continue?

A. Copper Kestrel expected the commercial documents to be resolved and understood testing to be permitted. Meridian followed Copper Kestrel's instructions and the technical conditions it received.

Q. Did Meridian make an independent legal determination that production use was licensed?

A. No. That was outside my role and outside Meridian's authority.

<!-- PAGE BREAK -->

## Examination — July 8 conference

Q. Did you attend the July 8 videoconference?

A. Yes, as the engineer responsible for reporting test status.

Q. What do you recall Asterglen's project manager saying?

A. I recall a statement that the teams could continue the current test while paperwork remained open, with monitoring and no additions beyond the existing configuration.

Q. Did you hear “production is authorized at all forty-one terminals”?

A. No one used that exact statement.

Q. Did you understand the instruction as permission to keep working?

A. Yes, for the current technical activity. I did not understand it as a signed amendment or a permanent license grant.

Q. Copper Kestrel's witness recalls permission to continue existing activity more broadly. Is that inconsistent?

A. It may reflect a different understanding of “current activity.” I focused on testing and integration tasks. Copper Kestrel controlled its operations.

Q. Asterglen denies approving expanded production. Do you dispute that its representative may have intended only restricted testing?

A. I cannot testify to another person's private intent. The notes are abbreviated and the call was not recorded.

Q. Did Meridian tell anyone afterward that the change order had been executed?

A. No. Our follow-up continued to show paperwork as pending.

Q. Is the scope of oral permission disputed?

A. Yes. My testimony should not be treated as agreement by all parties.

<!-- PAGE BREAK -->

## Examination — audit and technical assessment

Q. What happened after the July 18 compliance audit?

A. Copper Kestrel asked Meridian to help reconcile the list of forty-one identifiers with configuration and traffic data. Asterglen had questioned whether activity at the proposed locations exceeded the written test conditions.

Q. What did Meridian find?

A. The identifiers existed, and each had some configuration or access record. The volume and purpose varied. Several could be disabled without affecting the twelve licensed locations. Others required orderly session closure to avoid incomplete routing updates.

Q. Did the audit prove that every proposed terminal was in full production?

A. Not by identifier count alone. It supported further examination. Business-purpose classification required looking at event types, time periods, and Copper Kestrel's operations.

Q. Did Meridian receive a written fifteen-day cure instruction on July 18?

A. No.

Q. Was reduction technically possible?

A. Yes. We developed a sequence for freezing provisioning, mapping sessions, moving necessary activity, disabling the twenty-nine identifiers, and validating the remaining twelve.

Q. How long did the team estimate?

A. Approximately three business days with access to the necessary configuration information.

Q. Did Meridian communicate that it would refuse such work?

A. No. Reduction and validation were ordinary tasks within our technical ability.

<!-- PAGE BREAK -->

## Examination — July 22 through July 25

Q. When did you learn that Asterglen had disabled the shared credential?

A. On July 22. Copper Kestrel forwarded the termination and demand communication.

Q. What amount did Asterglen demand?

A. $480,000.

Q. Did the message allow fifteen calendar days for cure?

A. No. It declared immediate consequences.

Q. What did Meridian do next?

A. We stopped new provisioning, inventoried open sessions, preserved logs, assisted temporary routing, and worked with Copper Kestrel to reduce the configuration without corrupting shipment data.

Q. Did that work continue through July 25?

A. Yes.

Q. Was Meridian adding new terminals during those days?

A. No. The work concerned the existing configuration, mitigation, session closure, and validation.

Q. Could someone characterize any post-termination system access as continued use?

A. The logs show technical access. The reason for the access must be determined from the task tickets and testimony. Our tickets identify shutdown and continuity work.

Q. Did Meridian receive a new authorization from Asterglen on July 23, 24, or 25?

A. No.

Q. Did Meridian decide whether Section 9.2 excused Asterglen from offering cure?

A. No. We supplied technical information. Contract interpretation and litigation decisions were not engineering functions.

<!-- PAGE BREAK -->

## Examination — three-day test results

Q. Did Meridian participate in a preserved-configuration reduction test?

A. Yes. We used a copy of the configuration and representative session records to verify the planned sequence.

Q. Describe the sequence.

A. First, the team froze administrative additions. Second, it mapped sessions associated with each of the twenty-nine disputed identifiers. Third, necessary dispatch updates were transferred or completed through the original twelve. Fourth, the additional identifiers were disabled. Fifth, the team ran validation queries for data continuity and access boundaries.

Q. How long did that take?

A. Three business days from the controlled start through final validation.

Q. Did the recorded duration include unnecessary waiting inserted to lengthen the result?

A. No. The schedule reflects task completion and review. Some checks ran overnight, but the team worked within ordinary operational intervals.

Q. Did the result establish that cure would have been risk free?

A. No technical change is risk free. It established that reduction was feasible within substantially less than fifteen calendar days under the tested conditions.

Q. Would a written cure notice have helped coordinate the work?

A. A notice identifying the alleged breach, required end state, and due date would have clarified priorities and acceptance criteria.

Q. Did Meridian have that before July 22?

A. Not as a Section 9.2 cure instruction.

<!-- PAGE BREAK -->

## Examination — interference allegations and shared evidence

Q. Did Meridian tell Copper Kestrel to ignore Asterglen or conceal the identifier count?

A. No. Meridian reported technical status and followed Copper's directions. The table showed forty-one identifiers, and Asterglen's platform produced provisioning information. We knew the change order was unsigned, the June 10 test conditions existed, and the July 8 discussion was disputed.

Q. Did Meridian cause Copper to reject a cure opportunity?

A. No opportunity preceded termination. Meridian was prepared to reduce the configuration and later demonstrated a three-business-day sequence.

Q. Why does this testimony concern multiple claims?

A. The same communications and technical events bear on Count I, Count II, and Copper's $120,000 counterclaim, although their legal elements differ. Meridian's post-disablement work concerns interruption, mitigation, and restoration; its invoices bear on asserted costs, but I do not authenticate Copper's finance figures. No remaining liability had been decided when I testified.

## Authentication and filing use

Q. Are the June 14 statement of work, task tickets, and time entries Meridian materials?

A. Yes. Responsible personnel created them during the engagement. The July 8 permission remains disputed; I give only my own recollection.

Q. How was this excerpt filed?

A. It was attached as District ECF No. 49-3 to Asterglen's November 24, 2025 opposition. Filing it did not make Meridian an opponent of Copper's motion or make Asterglen adopt every answer.

The witness reviewed the excerpt for accuracy and reserved only ordinary transcription corrections. The deposition concluded at 2:57 p.m.
