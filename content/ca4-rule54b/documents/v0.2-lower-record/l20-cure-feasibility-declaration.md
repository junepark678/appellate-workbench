SYNTHETIC TRAINING RECORD — NOT FILED — ALL FACTS AND IDENTIFIERS ARE FICTIONAL

UNITED STATES DISTRICT COURT FOR THE NORTHERN DISTRICT OF WEST VIRGINIA

Proceeding: Cure-feasibility evidence.

Asterglen Freight Software, Inc., Plaintiff,
v.
Copper Kestrel Logistics, LLC and Meridian Silt Holdings, LLC, Defendants.

Civil Action No. SYN-NDWV-25-CV-0618

# Cure-Feasibility Declaration and Test Results

Filed November 24, 2025 as District ECF No. 49-4, an attachment to Asterglen's opposition at District ECF No. 49.

I am a systems-validation consultant retained to examine whether the forty-one-identifier BeaconRoute configuration could have been reduced to the twelve terminals named in the April 15, 2024 Pilot License. I have fifteen years of experience planning controlled access changes for routing and logistics systems. My assignment was technical. I offer no opinion on contract interpretation, whether any use was authorized, or whether a party was legally entitled to terminate.

I reviewed the July 1 provisioning ledger, the June 10 written load-test conditions, the July 18 audit, July 22 through July 25 task communications, authentication events, routing dependencies, and Meridian's configuration tickets. I also interviewed technical personnel for Copper Kestrel and Meridian about the steps actually available in July.

My testing asked a narrow question: assuming an instruction to return to the original twelve-terminal configuration, could personnel disable the twenty-nine additional identifiers, preserve completed shipment data, and validate continuing access within fifteen calendar days? The preserved environment and representative session data support an answer of yes. The controlled sequence completed within three business days.

<!-- PAGE BREAK -->

## Test environment and controls

The test used a preserved copy of the configuration listing all forty-one identifiers provisioned by July 1. The original twelve were marked as the required continuing set. The twenty-nine associated with the unsigned June 3 proposal were marked for removal. Configuration values were compared with contemporaneous task tickets and system reports before the test began.

Representative sessions were selected from the July event data to cover ordinary dispatch updates, routing queries, incomplete messages, and inactive identifiers. No new locations were introduced. The test did not assume that every recorded event was production use, and it did not decide the disputed scope of the July 8 permission. Its purpose was to reproduce the technical dependencies relevant to reduction.

The team recorded each administrative action, start and completion time, validation result, warning, and corrective step. Access was limited to the technical participants. The test began on a Monday morning and concluded Wednesday afternoon. Overnight automated checks were reviewed the next business morning.

The environment was not identical to a live July 22 system because ordinary business traffic could not be recreated perfectly. It nevertheless preserved the identifier relationships, representative session states, routing rules, and data-integrity checks needed to assess whether the proposed cure sequence was practicable.

<!-- PAGE BREAK -->

## First business day — freeze and mapping

The team first froze administrative provisioning so no identifier could be added while the reduction was underway. It then mapped each active or recently active session to one of the forty-one identifiers. The mapping separated the twelve continuing terminals from the twenty-nine scheduled for deactivation and identified any shared routing dependencies.

Most disputed identifiers could be disabled without affecting another terminal. Seven had open or recently incomplete dispatch updates requiring review. Four shared temporary routing references with one or more licensed terminals. The team did not disable those entries until the related updates were completed or transferred. This sequence avoided deleting stored shipment information.

By midafternoon, the team had classified every identifier and prepared a proposed action list. It notified the validation participant responsible for the twelve-terminal baseline and began transferring the small number of necessary open updates. No step required commercial approval of a new terminal. The work was consistent with a direction to restore the original licensed set.

At the end of the first business day, no new provisioning was possible, the complete dependency map had been checked, and twenty-two of the twenty-nine disputed identifiers were ready for immediate deactivation. The remaining seven required session closure or routing-reference cleanup scheduled for the following morning.

<!-- PAGE BREAK -->

## Second business day — migration and deactivation

The second day began with completion of the seven open-session reviews. Necessary dispatch information was transferred to an appropriate licensed terminal or marked complete after verification. Shared routing references were separated so disabling an additional identifier would not interrupt service at any of the twelve continuing locations.

The team then disabled the twenty-nine disputed identifiers in controlled groups. After each group, it ran authentication, routing, and data-retrieval checks against the continuing terminals. One validation warning identified a cached route still pointing to a deactivated test identifier. The team corrected the route and repeated the affected checks successfully.

No completed shipment entry was lost. No identifier outside the designated twenty-nine was disabled. The original twelve remained able to authenticate and process the representative routing tasks. The group sequence took longer than a single bulk command, but it supplied a reversible checkpoint after every change.

By late afternoon, the active configuration contained only the twelve terminals listed in the April 15 license schedule. The team preserved logs and snapshots needed for the final day of testing. The second-day result demonstrated that the principal technical cure—removing the additional identifiers while retaining the licensed service—was complete before the end of two business days.

<!-- PAGE BREAK -->

## Third business day — validation

On the third day, the team repeated authentication and routing tests for every continuing terminal. It queried completed and open shipment records, confirmed that transferred updates remained associated with the correct shipments, and checked that the twenty-nine disabled identifiers could no longer initiate a session.

The team also reviewed exception logs for unintended authorization failures, missing records, duplicate routing actions, and references to disabled locations. The prior cached-route warning did not recur. Two stale display entries appeared in a reporting view, but neither allowed access or affected dispatch. They were cleared and the view was refreshed.

Validation included a controlled restoration exercise for one disabled test identifier followed by immediate deactivation. That check confirmed that the action list and audit trail were sufficient to reverse a mistaken selection without altering unrelated data. The identifier ended the test disabled.

All acceptance criteria were satisfied Wednesday afternoon, within three business days of the Monday start. The resulting active set was twelve. The team retained the action log, before-and-after configuration lists, test queries, and exception report. The duration leaves a substantial margin within a fifteen-calendar-day cure period, even allowing additional review and communication time.

<!-- PAGE BREAK -->

## Relation to the July events

The July 18 audit showed forty-one provisioned identifiers and prompted review of whether activity exceeded Asterglen's written test conditions. The evidence I examined does not show that Asterglen delivered a written notice on July 18 directing reduction to twelve within fifteen days. Instead, Asterglen disabled a shared credential, declared termination, and demanded $480,000 on July 22.

Meridian's tickets from July 22 through July 25 describe tasks resembling the tested sequence: inventorying sessions, preventing new additions, protecting dispatch data, reducing access, and validating continuity. Those tickets are consistent with technical feasibility. They do not, by themselves, decide whether the post-July 22 work was authorized, mitigation, or continued breach.

The disputed July 8 conversation is also outside my ultimate opinion. If Asterglen permitted only controlled testing, reduction still could have been performed. If Copper Kestrel reasonably understood broader temporary permission, reduction likewise could have been performed after a written breach notice while the parties preserved their disagreement.

The test therefore bears on cure feasibility and the assertion that notice would have been futile. It does not establish that Copper Kestrel would certainly have complied, that every live transition would have been free of cost, or that Asterglen was required to accept any particular technical procedure.

<!-- PAGE BREAK -->

## Limitations and reliability

The preserved configuration accurately represented the identifier relationships and routing rules available for review. Representative sessions covered the material dependency types identified in the July information. I did not receive evidence of an undisclosed dependency that would necessarily extend reduction beyond fifteen calendar days.

The principal limitation is that the test occurred after the live events. Shipment volume, staffing, vendor response, and business urgency could have differed in July. I accounted for that limitation by using controlled groups, documenting the warning encountered, and avoiding an assumption that every step would succeed instantly. Even if each day of technical work required an additional review day, the sequence would remain inside fifteen calendar days under ordinary conditions.

I did not audit Copper Kestrel's $120,000 counterclaim or Asterglen's $480,000 demand. I did not assign fault for the July 22 disablement. I did not evaluate the legal effect of Section 9.2, the unsigned June 3 proposal, or oral statements attributed to July 8 participants.

My conclusion is limited to technical capacity: a documented, orderly return from forty-one identifiers to the original twelve was feasible in three business days in the tested environment, with completed shipment information preserved and continuing-terminal access validated.

<!-- PAGE BREAK -->

## Declaration and attached results summary

The results table identifies forty-one starting identifiers, twenty-nine selected for deactivation, twelve selected to continue, seven sessions requiring special review, one cached-route warning corrected during the sequence, and zero lost completed-shipment entries. The final authentication check succeeded for each of the twelve continuing terminals and failed as expected for each disabled identifier.

The action log records a Monday morning start, completion of mapping and freeze controls on the first day, migration and deactivation on the second day, and final validation on the third day. The table and log were prepared during the test from the observed actions and results. They fairly summarize the procedure I supervised.

I understand that this declaration was filed on November 24, 2025 as District ECF No. 49-4, attached to Asterglen's District ECF No. 49 opposition. Asterglen requested the analysis but did not direct me to assume its account of the July 8 call or to omit the test's warning and limitations.

I declare under penalty of perjury that the factual statements above are true and correct and that the opinions stated are held to a reasonable degree of technical certainty within the defined assignment. Executed on November 21, 2025.
