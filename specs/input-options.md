## API Usage use cases

1. 
Perona: A REactJS Single Page Applicatoin's human user using the web UI.
User downloads a source CSV and the modifies and uses a file upload button to attach the modified CSV
ReactJS prepares a callback method with calls inside it to retrieve SErver side original CSV, and provides both CSVs to the diff Engine


2. 
A backend java code receives a CSV file upload from REactJS SPA.
It then retrives the original CSV (or CSV stored in JSON format) from a CLOB or NVARCHAR(max) column and provides both to the diff engine along with Header row information.


3. 
A backend process is trying to compare 2 CSVs or (2 CSVs stored as JSON arrays) in CLOB or NVARCHAR(max) columns
It prepares the header information and retrieves both CSVs or JSON arrays from CLOB columsn and supplies to csv diff engine


4. 

A backend process provides two file paths to the diff engine (may be the file paths are some S3 paths or SAN disk paths where two CSV files are there). The CSVs contain header information in them. Diff ENgine has to read files and create diff

5. 
A java backend wants to run a ad-hoc SELECT statement to get rows of columns instead of a CLOB (as in scenario 2 and 3 ) and provide it to the DIFF engine along with header info , and then provide the second object as any of the first four options (1) File uploaded CSV (2) CSV BLob (3) CSV stored as JSON blob (4) CSV file path where the file contains header info


These are various ways data is ingested to the diff engine. Does the API structure provide for these scenarios? Which are presently possible and which are not possible.

The use cases are listed in the order in which features need to be delivered. 


