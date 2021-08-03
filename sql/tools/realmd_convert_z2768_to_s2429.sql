-- Query let convert realmd DB from format
-- CMaNGOS Classic realmd DB `required_z2768_01_realmd_account_locale_agnostic` to
-- CMaNGOS TBC realmd DB `required_s2429_01_characters_raf`.

-- Note: ALWAYS DO BACKUP before use it. You CAN NOT easy restore original DB state after tool use.

-- CONVERT DB VERSION
DROP TABLE IF EXISTS `realmd_db_version`;
CREATE TABLE `realmd_db_version` (
  `required_s2429_01_realmd_raf` bit(1) DEFAULT NULL
) ENGINE=MyISAM DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC COMMENT='Last applied sql update to DB';

LOCK TABLES `realmd_db_version` WRITE;
/*!40000 ALTER TABLE `realmd_db_version` DISABLE KEYS */;
INSERT INTO `realmd_db_version` VALUES
(NULL);
/*!40000 ALTER TABLE `realmd_db_version` ENABLE KEYS */;
UNLOCK TABLES;

-- ADD RAF TABLE
DROP TABLE IF EXISTS account_raf;
CREATE TABLE account_raf(
referrer INT UNSIGNED NOT NULL DEFAULT '0',
referred INT UNSIGNED NOT NULL DEFAULT '0',
PRIMARY KEY(referrer, referred)
);

-- UPGRADE ALL ACCOUNTS TO TBC
UPDATE `account`
  SET `expansion` = '1' WHERE `expansion` = '0';
