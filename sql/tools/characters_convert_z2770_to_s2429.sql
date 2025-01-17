-- Query let convert characters DB from format
-- CMaNGOS Classic characters DB `required_z2770_01_characters_item_instance_duration_default` to
-- CMaNGOS TBC characters DB `required_s2429_01_characters_raf`.

-- Note: ALWAYS DO BACKUP before use it. You CAN NOT easy restore original DB state after tool use.

-- CONVERT DB VERSION
DROP TABLE IF EXISTS `character_db_version`;
CREATE TABLE `character_db_version` (
  `required_s2429_01_characters_raf` bit(1) DEFAULT NULL
) ENGINE=MyISAM DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC COMMENT='Last applied sql update to DB';

LOCK TABLES `character_db_version` WRITE;
/*!40000 ALTER TABLE `character_db_version` DISABLE KEYS */;
INSERT INTO `character_db_version` VALUES
(NULL);
/*!40000 ALTER TABLE `character_db_version` ENABLE KEYS */;
UNLOCK TABLES;

-- CONVERT MAIN TABLES
ALTER TABLE `characters`
  ADD COLUMN `dungeon_difficulty` tinyint(1) unsigned NOT NULL DEFAULT '0' AFTER `map`,
  ADD COLUMN `arenaPoints` int(10) unsigned NOT NULL DEFAULT '0' AFTER `taxi_path`,
  DROP COLUMN `honor_standing`,
  DROP COLUMN `stored_honor_rating`,
  DROP COLUMN `stored_dishonorable_kills`,
  DROP COLUMN `stored_honorable_kills`,
  ADD COLUMN `totalHonorPoints` int(10) unsigned NOT NULL DEFAULT '0' AFTER `arenaPoints`,
  ADD COLUMN `todayHonorPoints` int(10) unsigned NOT NULL DEFAULT '0' AFTER `totalHonorPoints`,
  ADD COLUMN `yesterdayHonorPoints` int(10) unsigned NOT NULL DEFAULT '0' AFTER `todayHonorPoints`,
  ADD COLUMN `totalKills` int(10) UNSIGNED NOT NULL default '0' AFTER `yesterdayHonorPoints`,
  ADD COLUMN `todayKills` smallint(5) unsigned NOT NULL DEFAULT '0' AFTER `totalKills`,
  ADD COLUMN `yesterdayKills` smallint(5) unsigned NOT NULL DEFAULT '0' AFTER `todayKills`,
  ADD COLUMN `chosenTitle` int(10) unsigned NOT NULL DEFAULT '0' AFTER `yesterdayKills`,
  ADD COLUMN `knownTitles` longtext AFTER `ammoId`,
  ADD COLUMN `grantableLevels` INT UNSIGNED DEFAULT '0' AFTER `actionBars`;

ALTER TABLE `instance`
  ADD COLUMN `difficulty` tinyint(1) unsigned NOT NULL DEFAULT '0' AFTER `encountersMask`;

ALTER TABLE `item_loot`
  ADD COLUMN `suffix` int(11) unsigned NOT NULL DEFAULT '0' AFTER `amount`;

ALTER TABLE `character_social`
  ADD COLUMN `note` varchar(48) NOT NULL DEFAULT '' COMMENT 'Friend Note' AFTER `flags`;

ALTER TABLE `saved_variables`
  DROP COLUMN `NextMaintenanceDate`,
  ADD COLUMN `NextArenaPointDistributionTime` bigint(40) unsigned NOT NULL DEFAULT '0' FIRST,
  ADD COLUMN `NextDailyQuestResetTime` bigint(40) unsigned NOT NULL DEFAULT '0' AFTER `NextArenaPointDistributionTime`,
  ADD COLUMN `NextMonthlyQuestResetTime` bigint(40) unsigned NOT NULL DEFAULT '0' AFTER `NextWeeklyQuestResetTime`; 

ALTER TABLE `groups`
  ADD COLUMN `difficulty` tinyint(3) unsigned NOT NULL DEFAULT '0' AFTER `isRaid`;

ALTER TABLE `guild`
  ADD COLUMN `BankMoney` bigint(20) unsigned NOT NULL DEFAULT '0' AFTER `createdate`;

ALTER TABLE `guild_member`
  ADD COLUMN `BankResetTimeMoney` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankRemMoney` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankResetTimeTab0` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankRemSlotsTab0` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankResetTimeTab1` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankRemSlotsTab1` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankResetTimeTab2` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankRemSlotsTab2` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankResetTimeTab3` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankRemSlotsTab3` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankResetTimeTab4` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankRemSlotsTab4` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankResetTimeTab5` int(11) unsigned NOT NULL DEFAULT '0',
  ADD COLUMN `BankRemSlotsTab5` int(11) unsigned NOT NULL DEFAULT '0';

ALTER TABLE `guild_rank`
  ADD COLUMN `BankMoneyPerDay` int(11) unsigned NOT NULL DEFAULT '0' AFTER `rights`;

ALTER TABLE `petition`
  ADD COLUMN `type` int(10) unsigned NOT NULL DEFAULT '0' AFTER `name`,
  DROP PRIMARY KEY,
  ADD PRIMARY KEY (`ownerguid`,`type`);

ALTER TABLE `petition_sign`
  ADD COLUMN `type` int(10) unsigned NOT NULL DEFAULT '0' AFTER `player_account`;

-- SET ALL OLD PETITIONS TO GUILD PETITION TYPE
UPDATE `petition`
  SET `type` = 9;
UPDATE `petition_sign`
  SET `type` = 9;

-- RESET TALENTS
UPDATE `characters`
  SET `at_login` = `at_login` | '4' WHERE (at_login & '4') = '0';

-- CONVERT EXPLORED ZONES
UPDATE `characters`
  SET `exploredZones` = CONCAT(`exploredZones`, REPEAT('0 ', 64));

-- CLEAR AURAS AND COOLDOWN
DELETE FROM `character_spell_cooldown`;
DELETE FROM `pet_spell_cooldown`;
DELETE FROM `character_aura`;
DELETE FROM `pet_aura`;

-- REMOVE HONOR INFO
DROP TABLE IF EXISTS `character_honor_cp`;

-- SET EMPTY TITLE FIELD
UPDATE `characters`
  SET `knownTitles` = '0 0 ';

-- CONVERT PVP TITLES
-- ALLIANCE
UPDATE `characters`
  SET `knownTitles` = CONCAT((1 << ((`honor_highest_rank` - 4) % 32)), ' 0 ') WHERE `honor_highest_rank` > '0' AND `race` IN (1,3,4,7);
-- HORDE
UPDATE `characters`
  SET `knownTitles` = CONCAT((1 << (((`honor_highest_rank` - 4) + 14) % 32)), ' 0 ') WHERE `honor_highest_rank` > '0' AND `race` IN (2,5,6,8);

-- SET TITLE AS SELECTED
-- ALLIANCE
UPDATE `characters`
  SET `chosenTitle` = (`honor_highest_rank` - 4) WHERE `honor_highest_rank` > '0' AND `race` IN (1,3,4,7);
-- HORDE
UPDATE `characters`
  SET `chosenTitle` = ((`honor_highest_rank` - 4) + 14) WHERE `honor_highest_rank` > '0' AND `race` IN (2,5,6,8);

-- ADD SCARAB LORD TITLE IF GONG! QUEST COMPLETED
UPDATE `characters`
  SET `knownTitles` = REPLACE(`knownTitles`, ' 0 ', ' 2 ') WHERE `guid` IN (SELECT `guid` FROM `character_queststatus` WHERE `quest` = '8743' AND `rewarded` = '1');

-- SET SCARAB LORD TITLE AS SELECTED
UPDATE `characters`
  SET `chosenTitle` = '33' WHERE `guid` IN (SELECT `guid` FROM `character_queststatus` WHERE `quest` = '8743' AND `rewarded` = '1');

-- DROP RANK INFO AFTER TITLE TRANSFER
ALTER TABLE `characters`
  DROP COLUMN `honor_highest_rank`;

-- DROP WORLD STATE INFO  
DROP TABLE IF EXISTS world_state;
CREATE TABLE world_state(
   Id INT(11) UNSIGNED NOT NULL COMMENT 'Internal save ID',
   Data longtext,
   PRIMARY KEY(`Id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=COMPACT COMMENT='WorldState save system';
   